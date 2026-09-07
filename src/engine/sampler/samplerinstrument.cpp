//----------------------------------------------------------------------------
//  src/engine/sampler/samplerinstrument.cpp
//
//  Self-contained POLYPHONIC SAMPLER instrument (IPluginInstance).  No Buzz /
//  Unwieldy dependency: a purpose-built voice engine so voices are truly
//  ISOLATED -- each MIDI note owns a voice with its own sample position, pitch,
//  gain and modulation envelopes, and nothing is shared or global.  A note in
//  one voice can NEVER disturb (re-trigger / reposition) another voice, which the
//  old tracker-machine backend could not guarantee (its shared per-track "row"
//  state leaked: e.g. a global Offset param reset every held voice's position).
//
//  Features: multisample zones (keyrange + velocity layer + nearest-root pick),
//  cubic-interpolated resampling, per-voice ADSR-style amp/pitch/cutoff/res/pan
//  envelopes (piecewise-linear over the editor's dense points, held at sustain),
//  loop / one-shot, voice stealing, and a native FX tail (gain/pan/width/drive/
//  LP/HP/bitcrush/downsample/noise).  State (samples + zones + envelopes + params)
//  round-trips through saveState()/loadState().
//----------------------------------------------------------------------------
#include "sampler_instrument.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace PatchKnob { namespace engine {

namespace {
constexpr int   kMaxVoices   = 32;   // note voices the allocator hands out
// Declick tails live in their OWN reserved slots.  If a retriggered voice had to
// compete with live notes for somewhere to fade out, the one case that needs the
// crossfade most -- a steal at full polyphony -- would be the one case that never
// gets it, and the clicks would come straight back at high note rates.
constexpr int   kMaxTails    = 8;
constexpr int   kTotalVoices = kMaxVoices + kMaxTails;
constexpr int   kMaxColumns  = 8;    // tracker note-columns; per-column voice + FX
constexpr int   kDefaultSlot = 1;
constexpr int   kNumEnvs     = 5;    // 0 amp, 1 pitch, 2 cutoff, 3 resonance, 4 pan
constexpr float kEnvSeconds  = 2.0f;
// Longest single envelope stage.  SoundFont timecents top out at 8000, i.e.
// 2^(8000/1200) = 101.6 s, and real banks use it: a concert grand's decay is
// ~100 s.  The old 60 s clamp silently truncated those on import, which is
// heard as a piano that dies half way through its own decay.
constexpr float kMaxEnvStageSec = 120.0f; // envelope x=0..1 spans this many seconds
constexpr float kSustainFlag = 1.f;  // EIF_SUSTAIN

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
inline float db_to_gain(float db) { return std::pow(10.f, db / 20.f); }
inline float onepole_alpha(float hz, double sr) {
    if (hz < 10.f) hz = 10.f;
    if (hz > (float)sr * 0.45f) hz = (float)sr * 0.45f;
    return 1.f - std::exp(-2.f * 3.1415926535f * hz / (float)sr);
}
// semitone -> playback-rate ratio
inline double semisToRatio(double semis) { return std::pow(2.0, semis / 12.0); }

//! N_OUTPUT_GAIN (0..1) -> linear gain.  ONE curve for the parameter, wherever
//! it is applied: the instrument-global bus gain in applyNativeFx() reads it as
//! -24..+24 dB with 0.5 == unity, but the per-COLUMN copy used the normalised
//! value as a raw linear multiplier -- so a tracker volume column sitting at its
//! own default (0.5) played that column 6 dB down from every other one.
//! The bottom of the range is a true MUTE, not -24 dB: a tracker volume column
//! set to zero has to silence its column (and the same fader at its stop should
//! silence the instrument), which a pure dB curve never reaches.
inline float volumeNormToGain(float norm) {
    norm = clamp01(norm);
    return norm <= 0.f ? 0.f : db_to_gain(norm * 48.f - 24.f);
}

//! Declick ramp shape (smoothstep).  Two properties are load-bearing:
//!   * s'(0) == s'(1) == 0, so the ramp adds no corner of its own at either end
//!     (a plain linear fade still leaves a slope discontinuity you can hear as a
//!     soft tick at very short fade times);
//!   * s(t) + s(1-t) == 1 exactly, so an outgoing voice fading out and an
//!     incoming voice fading in over the SAME window sum to unity gain -- that
//!     is what makes the retrigger a true crossfade rather than a dip.
inline float declickShape(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    return t * t * (3.f - 2.f * t);
}

//! Hann-ish grain window built from the same complementary shape: a triangular
//! phase through declickShape().  At 50% overlap consecutive grains sum to 1.0
//! (COLA), so the Akai smear has no amplitude ripple -- and no cosf() per sample.
inline float grainWindow(float t) {
    if (t <= 0.f || t >= 1.f) return 0.f;
    return t < 0.5f ? declickShape(t * 2.f) : declickShape((1.f - t) * 2.f);
}
} // namespace

// One point of a modulation envelope (editor's dense representation).
struct EnvPointF { float x = 0.f, y = 0.f; bool sustain = false; };
struct EnvelopeF {
    std::vector<EnvPointF> pts;
    int  sustainIdx = -1;
    bool enabled    = false;
    //! How many SECONDS the 0..1 phase axis covers.  It used to be the fixed
    //! kEnvSeconds for every envelope, which is why a second, stage-based
    //! envelope had to be bolted on for SoundFont import: a piano's 100-second
    //! decay simply cannot be drawn on a 2-second axis.  Carrying the span here
    //! means ONE drawable representation covers both -- arbitrary shapes AND
    //! unbounded times -- and the second evaluator goes away.
    float spanSec   = kEnvSeconds;
    // Evaluate at normalized phase (0..1), piecewise-linear.  Empty -> 1.0.
    float eval(double phase) const {
        if (pts.empty()) return 1.f;
        if (phase <= pts.front().x) return pts.front().y;
        if (phase >= pts.back().x)  return pts.back().y;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const EnvPointF& a = pts[i];
            const EnvPointF& b = pts[i + 1];
            if (phase >= a.x && phase <= b.x) {
                const float span = b.x - a.x;
                const float t = span > 1e-9f ? (float)((phase - a.x) / span) : 0.f;
                return a.y + (b.y - a.y) * t;
            }
        }
        return pts.back().y;
    }
    //! eval() with a caller-held SEGMENT HINT.  eval() scans the point list
    //! from the start on every call, which is O(points) PER SAMPLE PER VOICE on
    //! the audio thread -- measured at 186 ns/call for a 128-point hand-drawn
    //! envelope, i.e. ~6 us/sample with 32 voices, 28% of the whole realtime
    //! budget at 48 kHz (a 512-point curve exceeds the budget outright).  A
    //! voice's phase is monotonic (it only ever advances, or jumps FORWARD to
    //! the sustain point at note-off), so remembering the segment makes the
    //! walk amortised O(1).  The hint self-heals: any phase below the hinted
    //! segment (voice retrigger, envelope swapped under the voice by the
    //! editor) restarts the scan from 0, so the result is IDENTICAL to eval()
    //! for every input -- the hint only changes where the search starts.
    float evalHinted(double phase, int& hint) const {
        const int n = (int)pts.size();
        if (n == 0) return 1.f;
        if (phase <= pts[0].x) { hint = 0; return pts[0].y; }
        if (phase >= pts[(size_t)(n - 1)].x) { hint = n > 1 ? n - 2 : 0; return pts[(size_t)(n - 1)].y; }
        int i = hint;
        if (i < 0 || i > n - 2 || phase < pts[(size_t)i].x) i = 0;
        while (i + 1 <= n - 2 && phase > pts[(size_t)(i + 1)].x) ++i;
        hint = i;
        const EnvPointF& a = pts[(size_t)i];
        const EnvPointF& b = pts[(size_t)(i + 1)];
        const float span = b.x - a.x;
        const float t = span > 1e-9f ? (float)((phase - a.x) / span) : 0.f;
        return a.y + (b.y - a.y) * t;
    }
    float sustainX() const {
        return (sustainIdx >= 0 && sustainIdx < (int)pts.size()) ? pts[(size_t)sustainIdx].x : 1.f;
    }

    //! Build a drawable envelope from SoundFont DAHDSR stages, so an imported
    //! patch lands in the SAME representation the user edits by hand: delay and
    //! attack ramp to full, hold stays there, decay falls to the sustain LEVEL
    //! (flagged as the sustain point, which is what holds the note until
    //! release), then release falls to silence.  Nothing about SF2 is lost, and
    //! everything about it is subsequently editable.
    static EnvelopeF fromStages(const SamplerZoneEnv& e) {
        EnvelopeF out;
        if (!e.enabled) return out;
        const float d = std::max(0.f, e.delay),  a = std::max(0.f, e.attack);
        const float h = std::max(0.f, e.hold),   dc = std::max(0.f, e.decay);
        const float r = std::max(0.f, e.release);
        const float sus = std::max(0.f, std::min(1.f, e.sustain));

        float t = 0.f;
        // Only start from silence if there is actually a delay or an attack to
        // rise through.  An instant-attack envelope (SF2's default, and most
        // percussion) must start AT full level: emitting a (0,0) point before a
        // (0,1) point puts two points on the same x, and eval() answers with the
        // first one -- so the whole zone plays silence.
        if (d > 0.f || a > 0.f) {
            out.pts.push_back({0.f, 0.f, false});
            if (d > 0.f) { t = d; out.pts.push_back({t, 0.f, false}); }
            t += a;
        }
        out.pts.push_back({t, 1.f, false});
        if (h > 0.f) { t += h; out.pts.push_back({t, 1.f, false}); }
        t += dc; out.pts.push_back({t, sus, true});
        out.sustainIdx = (int)out.pts.size() - 1;
        t += r;  out.pts.push_back({t, 0.f, false});

        // The x axis stays normalised 0..1 (every existing consumer expects
        // that); spanSec carries the real duration.
        out.spanSec = std::max(t, 1.0e-3f);
        for (EnvPointF& pt : out.pts) pt.x /= out.spanSec;
        // Coincident points elsewhere (a zero-length hold or decay) would hide
        // the later value the same way; keep x strictly increasing.
        for (size_t i = 1; i < out.pts.size(); ++i)
            if (out.pts[i].x <= out.pts[i-1].x)
                out.pts[i].x = out.pts[i-1].x + 1.0e-6f;
        if (!out.pts.empty() && out.pts.back().x > 1.f)
            for (EnvPointF& pt : out.pts) pt.x /= out.pts.back().x;
        out.enabled = true;
        return out;
    }
};

struct SampleZone {
    int slot = kDefaultSlot, level = 0;
    int rootKey = 60, loKey = 0, hiKey = 127, loVel = 0, hiVel = 127;
    bool noteOffLayer = false, keyToPitch = true, velToVol = true;
    int overlapMode = 0;
    int sampleRate = 48000, loopStart = 0, loopEnd = 0;
    bool loop = false, stereo = false;
    int numFrames = 0;
    std::string name;
    // PCM IS SHARED AND IMMUTABLE.  Multisampled instruments reuse one recording
    // across velocity layers and round-robins, so zones sharing a sample is the
    // NORMAL case: a 2976-zone / 192-sample piano preset that owns its audio
    // outright costs ~1.3 GB where the distinct samples are ~85 MB.  A
    // shared_ptr to a CONST buffer makes N zones one allocation, and const means
    // no zone can ever mutate audio another zone is reading.
    SharedPcm pcm;            // interleaved -1..1 (L,R,... if stereo)
    // ---- per-zone parameters (SF2 generator parity) ------------------------
    // A zone whose envelope is `enabled == 0` falls back to the instrument's --
    // that is the default, so every pre-v5 project sounds exactly as it did.
    SamplerZoneEnv ampEnv, modEnv;
    //! The SAME envelopes in the editor's drawable form.  The stage values above
    //! stay so get_zone_env still round-trips exactly what was set (and so an
    //! SF2 export can hand back stages); these are what the VOICE plays, which
    //! is what makes hand-drawn and imported envelopes one system rather than
    //! two that can disagree.
    EnvelopeF ampEnvPts, modEnvPts;
    float cutoffHz = 0.f;         // initialFilterFc; 0 = no filter
    float resonanceDb = 0.f;      // initialFilterQ
    int   coarseTune = 0;         // semitones
    int   fineTune = 0;           // cents
    int   scaleTuning = 100;      // cents per key; 0 = fixed pitch (drums)
    float pan = 0.f;              // -1..+1
    float attenuationDb = 0.f;    // positive = quieter
    int   exclusiveClass = 0;     // 0 = none
    float modEnvToPitchCents = 0.f, modEnvToFilterCents = 0.f;
    inline void frame(double pos, float& l, float& r) const;  // cubic read
};

// Catmull-Rom cubic interpolation of one channel at fractional frame `pos`.
static inline float cubicCh(const float* s, int n, int stride, double pos) {
    int i = (int)pos; double f = pos - i;
    auto at = [&](int k) -> float { if (k < 0) k = 0; if (k >= n) k = n - 1; return s[(size_t)k * stride]; };
    float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    float b =        y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c = -0.5f * y0             + 0.5f * y2;
    return (float)(((a * f + b) * f + c) * f + y1);
}
void SampleZone::frame(double pos, float& l, float& r) const {
    if (!pcm || pcm->empty() || numFrames <= 0) { l = r = 0.f; return; }
    const float* d = pcm->data();
    if (stereo) { l = cubicCh(d,     numFrames, 2, pos);
                  r = cubicCh(d + 1, numFrames, 2, pos); }
    else        { l = r = cubicCh(d, numFrames, 1, pos); }
}

class SamplerInstrument : public IPluginInstance {
public:
    SamplerInstrument() {
        desc_.format = PluginFormat::VST2;
        desc_.name = "Sampler"; desc_.vendor = "PatchKnob";
        desc_.isInstrument = true; desc_.numAudioIn = 0; desc_.numAudioOut = 2;
        for (int i = 0; i < 128; ++i) noteColumn_[i] = -1;
        for (int i = 0; i < N_COUNT; ++i) { native_[i] = nativeDefault(i); nativeSeen_[i] = native_[i]; }
        for (int c = 0; c < kMaxColumns; ++c) {
            colGain_[c] = 1.f; colPan_[c] = 0.f; colPitch_[c] = 1.0;
            colSet_[c] = 0u;
            for (int i = 0; i < N_COUNT; ++i) colFx_[c][i] = native_[i];
        }
    }
    ~SamplerInstrument() override {}

    std::mutex& machineMutex() { return mutex_; }

    const PluginDescriptor& descriptor() const override { return desc_; }

    bool prepare(double sampleRate, int maxBlock) override {
        std::lock_guard<std::mutex> lk(mutex_);
        sr_ = sampleRate > 0 ? sampleRate : 48000.0;
        for (Voice& v : voices_) v = Voice{};
        (void)maxBlock;
        return true;
    }
    void setActive(bool) override {}
    void release() override {}

    // ---- realtime -----------------------------------------------------------
    void process(const ProcessBlock& blk) override {
        if (!blk.audioOut || blk.numAudioOut < 1) return;
        std::lock_guard<std::mutex> lk(mutex_);
        const int n = blk.nframes;
        for (int i = 0; i < blk.numParamIn; ++i)
            setParamNormalized(blk.paramIn[i].id, blk.paramIn[i].value);
        syncGlobalParams();

        float* L = blk.audioOut[0];
        float* R = (blk.numAudioOut > 1 && blk.audioOut[1]) ? blk.audioOut[1] : nullptr;
        for (int i = 0; i < n; ++i) { L[i] = 0.f; if (R) R[i] = 0.f; }

        // Render voices between MIDI events -- each event is applied at its exact
        // sample offset and touches ONLY its own voice, so voices stay isolated.
        int pos = 0, mi = 0;
        while (pos < n) {
            while (mi < blk.numMidiIn && blk.midiIn[mi].sampleOffset <= pos) {
                handleMidi(blk.midiIn[mi]); ++mi;
            }
            int next = n;
            if (mi < blk.numMidiIn && blk.midiIn[mi].sampleOffset > pos)
                next = std::min(n, (int)blk.midiIn[mi].sampleOffset);
            renderVoices(L, R, pos, next - pos);
            pos = next;
        }
        // Anything the walk above could not place -- an offset at or past the end
        // of the block -- would otherwise be silently DROPPED when the loop exits
        // at pos == n, losing note-ONs and, far worse, note-OFFs.  The engine
        // clamps offsets into range before it gets here, so this is a guard
        // against a misbehaving host rather than a live path, but a lost note-off
        // hangs a voice forever and that is not worth leaving to trust.
        for (; mi < blk.numMidiIn; ++mi) handleMidi(blk.midiIn[mi]);

        applyNativeFx(L, R, n);
    }

    // ---- params (native only; the tracker FX picker automates these) --------
    int       paramCount() const override { return N_COUNT; }
    ParamInfo paramInfo(int index) const override {
        ParamInfo pi{};
        if (index >= 0 && index < N_COUNT) {
            pi.id = (uint32_t)index; pi.name = nativeName(index);
            pi.defaultValue = nativeDefault(index);
        }
        return pi;
    }
    float getParamNormalized(uint32_t id) const override {
        return id < (uint32_t)N_COUNT ? native_[id] : 0.f;
    }
    void setParamNormalized(uint32_t id, float v) override {
        if (id < (uint32_t)N_COUNT) native_[id] = clamp01(v);
    }

    // ---- per-column FX (built-in only; not part of the frozen plugin contract) --
    // A tracker FX column tagged with a note-column drives ONE column's voices.
    // Only the per-voice-safe targets (volume / pan / pitch) are per-column; any
    // other id (filters, drive, ...) still applies globally.  column<0 == global.
    void setColumnParam(uint32_t id, float value, int column) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (id >= (uint32_t)N_COUNT) return;
        value = clamp01(value);
        if (column < 0 || column >= kMaxColumns) {   // global write
            native_[id] = value;
            return;                                  // process() propagates it
        }
        // Record the column's own value and mark the id as column-owned.  It is
        // deliberately NOT written to native_[]: that global write was the leak
        // -- automating one column's downsample or offset changed the value every
        // other column and every other keyzone read from.
        colFx_[column][id] = value;
        colSet_[column] |= (1u << id);
        // Reach exactly the voices tagged with this column, and nothing else.
        for (Voice& v : voices_) {
            if (!v.active || v.fadingOut || v.column != column) continue;
            v.fx[id] = value;
            v.fxMask |= (1u << id);
            // Sample Offset is a tracker PERFORMANCE command, not merely the
            // default for the next trigger.  Tracker rows are dispatched from
            // the playhead/UI side and can arrive just after the note-on that
            // shares the row.  Previously we updated v.fx[] but left v.pos
            // untouched in clean mode, so the command visibly did nothing.
            // Retarget every sounding voice owned by this note column now:
            // clean mode jumps, Akai mode lets the grain path travel there.
            if (id == (uint32_t)N_OFFSET && v.zone) {
                v.offsetSeen = value;
                const double target = startOffset(*v.zone, v);
                v.targetOffset = target;
                if (v.fx[N_OFFSET_MODE] < 0.5f) {
                    v.pos = target;
                    v.baseOffset = target;
                    v.smearShift = 0.0;
                }
            }
        }
        // gain / pan / pitch additionally drive the dedicated per-voice controls.
        if (id == (uint32_t)N_OUTPUT_GAIN) {
            const float g = volumeNormToGain(value);
            colGain_[column] = g;
            for (Voice& v : voices_) if (v.active && !v.fadingOut && v.column == column) v.modGain = g;
        } else if (id == (uint32_t)N_PAN) {
            const float p = value * 2.f - 1.f; colPan_[column] = p;
            for (Voice& v : voices_) if (v.active && !v.fadingOut && v.column == column) v.modPan = p;
        } else if (id == (uint32_t)N_TRANSPOSE) {
            const double ratio = semisToRatio((value - 0.5) * 48.0); colPitch_[column] = ratio;
            // modPitch is what this column adds ON TOP of the transposition the
            // voice already carries in v.step (v.pitchBaked), never the whole
            // transposition again -- see noteOn().
            for (Voice& v : voices_) if (v.active && !v.fadingOut && v.column == column)
                v.modPitch = (v.zone && !v.zone->keyToPitch) ? 1.0 : ratio / v.pitchBaked;
        }
    }
    // Tell the sampler which tracker column a MIDI note belongs to (set by the
    // engine just before that note's note-on).  benign single-byte cross-thread.
    void setNoteColumn(int note, int column) {
        if (note >= 0 && note < 128)
            noteColumn_[note] = (int8_t)((column < 0) ? -1 : (column > 127 ? 127 : column));
    }

    bool hasEditor() const override { return false; }
    bool openEditor(NativeWindowHandle) override { return false; }
    void closeEditor() override {}
    void getEditorSize(int& w, int& h) const override { w = 0; h = 0; }
    void idleEditor() override {}

    // ---- state --------------------------------------------------------------
    std::vector<uint8_t> saveState() const override {
        // SNAPSHOT under the lock, SERIALIZE outside it.  Serialising the PCM
        // while holding mutex_ blocked process() -- which takes the same mutex
        // -- for the whole save (measured 102 ms over a synthetic 36 MB bank;
        // seconds over a real 500 MB one, i.e. a guaranteed audio dropout on
        // every project save / autosave).  Copying the zone METADATA plus
        // shared_ptr references costs ~a millisecond and no PCM is duplicated:
        // the buffers are immutable, so reading them unlocked is safe, and the
        // refs keep them alive even if the instrument drops them mid-save.
        std::vector<SampleZone> zsnap;
        float                   nsnap[N_COUNT];
        EnvelopeF               esnap[kNumEnvs];
        {
            std::lock_guard<std::mutex> lk(mutex_);
            zsnap = zones_;
            std::memcpy(nsnap, native_, sizeof nsnap);
            for (int e = 0; e < kNumEnvs; ++e) esnap[e] = envs_[e];
        }
        std::vector<uint8_t> out;
        // Reserve the (over-)estimated final size up front: growing a
        // multi-megabyte blob by push_back re-copies it O(log n) times.
        {
            size_t est = 64 + (size_t)N_COUNT * 4;
            std::unordered_set<const void*> seen;
            for (const SampleZone& z : zsnap) {
                est += 200 + z.name.size();
                if (z.pcm && seen.insert(z.pcm.get()).second)
                    est += 8 + z.pcm->size() * 4;
            }
            for (int e = 0; e < kNumEnvs; ++e) est += 8 + esnap[e].pts.size() * 12;
            out.reserve(est);
        }
        auto p32 = [&](uint32_t v){ for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (i * 8))); };
        auto pf  = [&](float f){ uint32_t v; std::memcpy(&v, &f, 4); p32(v); };
        auto ps  = [&](const std::string& s){ p32((uint32_t)s.size()); out.insert(out.end(), s.begin(), s.end()); };
        auto penv = [&](const SamplerZoneEnv& e){
            p32(e.enabled ? 1u : 0u);
            pf(e.delay); pf(e.attack); pf(e.hold); pf(e.decay);
            pf(e.sustain); pf(e.release);
        };
        p32(0x53504d53u);                 // "SMPS"
        // version 6: each zone record carries a SHARE INDEX ahead of its PCM.
        //   shareIdx == this zone's index -> the sample data follows inline
        //   shareIdx <  this zone's index -> reuse the buffer that zone wrote
        // Without it the 15x in-memory blowup this version exists to remove
        // would simply reappear in the project file: a 2976-zone piano preset
        // would write its 192 distinct samples 2976 times.
        //
        // v5 added the per-zone envelope/filter/tuning tail; v3/v4 predate it.
        // All three still load -- see loadState().
        p32(6);
        p32((uint32_t)N_COUNT); p32(0);   // nativeCount, buzzCount(=0, legacy slot)
        for (int i = 0; i < N_COUNT; ++i) pf(nsnap[i]);
        p32((uint32_t)zsnap.size());
        // Identity, not value: two zones share a buffer when they point at the
        // SAME allocation.  That is exactly what the loader can restore, and it
        // costs one pointer compare instead of hashing gigabytes of audio.
        // Only DISTINCT buffers go in the table (192 entries for a 2976-zone
        // piano), so the scan stays short rather than growing with zone count.
        std::vector<std::pair<const std::vector<float>*, uint32_t>> distinct;
        size_t zi = 0;
        for (const SampleZone& z : zsnap) {
            p32((uint32_t)z.slot); p32((uint32_t)z.level);
            p32((uint32_t)z.rootKey); p32((uint32_t)z.loKey); p32((uint32_t)z.hiKey);
            p32((uint32_t)z.loVel); p32((uint32_t)z.hiVel);
            p32(z.noteOffLayer ? 1u : 0u); p32(z.keyToPitch ? 1u : 0u);
            p32(z.velToVol ? 1u : 0u); p32((uint32_t)z.overlapMode);
            p32((uint32_t)z.sampleRate); p32((uint32_t)z.loopStart); p32((uint32_t)z.loopEnd);
            p32(z.loop ? 1u : 0u); p32(z.stereo ? 1u : 0u); p32((uint32_t)z.numFrames);
            ps(z.name);
            // ---- v6 shared-PCM reference ------------------------------------
            const std::vector<float>* key = z.pcm.get();
            uint32_t shareIdx = (uint32_t)zi;
            for (const auto& d : distinct)
                if (d.first == key) { shareIdx = d.second; break; }
            if (shareIdx == (uint32_t)zi) {          // first use: data inline
                distinct.emplace_back(key, (uint32_t)zi);
                p32(shareIdx);
                p32((uint32_t)(z.pcm ? z.pcm->size() : 0u));
                if (z.pcm && !z.pcm->empty()) {
                    // Bulk write on little-endian hosts (both shipped targets,
                    // Linux x86-64 and Windows MinGW x86-64, are LE): one
                    // insert instead of four push_backs per float.  The
                    // byte-wise fallback keeps the on-disk format identical
                    // anywhere else.
                    const uint32_t probe = 1u;
                    if (*(const uint8_t*)&probe == 1u) {
                        const uint8_t* p = (const uint8_t*)z.pcm->data();
                        out.insert(out.end(), p, p + z.pcm->size() * 4u);
                    } else {
                        for (float sm : *z.pcm) pf(sm);
                    }
                }
            } else {
                p32(shareIdx);                       // reuse: no data at all
            }
            ++zi;
            // ---- v5 per-zone tail -------------------------------------------
            penv(z.ampEnv); penv(z.modEnv);
            pf(z.cutoffHz); pf(z.resonanceDb);
            p32((uint32_t)(int32_t)z.coarseTune); p32((uint32_t)(int32_t)z.fineTune);
            p32((uint32_t)(int32_t)z.scaleTuning);
            pf(z.pan); pf(z.attenuationDb);
            p32((uint32_t)(int32_t)z.exclusiveClass);
            pf(z.modEnvToPitchCents); pf(z.modEnvToFilterCents);
        }
        p32((uint32_t)kNumEnvs);
        for (int e = 0; e < kNumEnvs; ++e) {
            p32((uint32_t)esnap[e].pts.size());
            for (const EnvPointF& pt : esnap[e].pts) {
                pf(pt.x); pf(pt.y); p32(pt.sustain ? 1u : 0u);
            }
        }
        return out;
    }
    void loadState(const std::vector<uint8_t>& d) override {
        size_t at = 0;
        auto g32 = [&]() -> uint32_t {
            if (at + 4 > d.size()) return 0;
            uint32_t v = (uint32_t)d[at] | ((uint32_t)d[at+1] << 8) |
                         ((uint32_t)d[at+2] << 16) | ((uint32_t)d[at+3] << 24);
            at += 4; return v;
        };
        auto gf  = [&]() -> float { uint32_t v = g32(); float f = 0.f; std::memcpy(&f, &v, 4); return f; };
        auto gs  = [&]() -> std::string {
            uint32_t len = g32();
            if (at + len > d.size()) { at = d.size(); return {}; }
            std::string s((const char*)d.data() + at, len); at += len; return s;
        };
        auto genv = [&]() -> SamplerZoneEnv {
            SamplerZoneEnv e;
            e.enabled = (int)(g32() != 0u);
            e.delay = gf(); e.attack = gf(); e.hold = gf(); e.decay = gf();
            e.sustain = gf(); e.release = gf();
            return e;
        };
        if (g32() != 0x53504d53u) return;
        uint32_t ver = g32();
        if (ver < 1 || ver > 6) return;
        uint32_t nativeCount = g32();
        uint32_t buzzCount   = g32();
        for (uint32_t i = 0; i < nativeCount && at + 4 <= d.size(); ++i) {
            float v = gf(); if (i < (uint32_t)N_COUNT) native_[i] = clamp01(v);
        }
        // legacy (v<=3) Buzz FX-param norms: skip -- this engine has no Buzz params.
        for (uint32_t i = 0; i < buzzCount && at + 4 <= d.size(); ++i) (void)gf();

        std::vector<SampleZone> zs;
        // Buffers keyed by the BLOB's zone index, so a v6 share reference
        // resolves even when an earlier zone was rejected and never made it into
        // `zs` -- indices must mean what the writer meant, not what survived.
        std::vector<SharedPcm> bufs;
        if (ver >= 2) {
            uint32_t zc = g32();
            // A real SoundFont preset runs to thousands of zones (a Concert
            // Grand measured at 2976), so the old 512 guard would have rejected
            // exactly the banks shared buffers exist to make loadable.  Still
            // bounded, just at a number that is absurd rather than merely large.
            if (zc > 65536u) return;
            bufs.resize(zc);
            for (uint32_t zi = 0; zi < zc && at < d.size(); ++zi) {
                SampleZone z;
                z.slot = (int)g32(); z.level = (int)g32();
                z.rootKey = (int)g32(); z.loKey = (int)g32(); z.hiKey = (int)g32();
                z.loVel = (int)g32(); z.hiVel = (int)g32();
                z.noteOffLayer = g32() != 0; z.keyToPitch = g32() != 0;
                z.velToVol = g32() != 0; z.overlapMode = (int)g32();
                z.sampleRate = (int)g32(); z.loopStart = (int)g32(); z.loopEnd = (int)g32();
                z.loop = g32() != 0; z.stereo = g32() != 0; z.numFrames = (int)g32();
                z.name = gs();
                // v6 carries a share index; older versions always inline.
                const uint32_t shareIdx = (ver >= 6) ? g32() : zi;
                if (shareIdx != zi) {
                    // Re-share, do not re-read: this is what keeps a 2976-zone
                    // preset at its 192 allocations after a project load.
                    if (shareIdx >= zi) return;          // forward/self ref: corrupt
                    z.pcm = bufs[shareIdx];
                } else {
                    uint32_t ns = g32();
                    if (ns > 256u * 1024u * 1024u || at + (size_t)ns * 4u > d.size()) return;
                    auto buf = std::make_shared<std::vector<float>>();
                    buf->resize(ns);
                    // Bulk read on little-endian hosts (bounds already checked
                    // above); byte-wise fallback preserves the format elsewhere.
                    const uint32_t probe = 1u;
                    if (ns > 0 && *(const uint8_t*)&probe == 1u) {
                        std::memcpy(buf->data(), d.data() + at, (size_t)ns * 4u);
                        at += (size_t)ns * 4u;
                    } else {
                        for (uint32_t i = 0; i < ns; ++i) (*buf)[i] = gf();
                    }
                    z.pcm = std::move(buf);
                }
                bufs[zi] = z.pcm;
                // v3/v4 zones simply have no tail here: every per-zone field
                // keeps its default, so the zone falls back to the
                // instrument-global envelope and sounds exactly as it did.
                if (ver >= 5) {
                    z.ampEnv = genv(); z.modEnv = genv();
                    z.cutoffHz = gf(); z.resonanceDb = gf();
                    z.coarseTune = (int)(int32_t)g32(); z.fineTune = (int)(int32_t)g32();
                    z.scaleTuning = (int)(int32_t)g32();
                    z.pan = gf(); z.attenuationDb = gf();
                    z.exclusiveClass = (int)(int32_t)g32();
                    z.modEnvToPitchCents = gf(); z.modEnvToFilterCents = gf();
                }
                sanitizeZone(z);
                // Rebuild the DRAWABLE envelopes from the restored stages
                // (after sanitize, same order as setZoneEnv).  Only setZoneEnv
                // ever called fromStages, so a loaded project kept the stages
                // -- get_zone_env read them back fine -- while PLAYBACK ignored
                // them: every restored zone fell back to the instrument
                // envelope.  Audible bug, and with an imported bank whose
                // zones decay to silence it is also a perf drain: voices that
                // should die sustain at full render cost until stolen.
                z.ampEnvPts = EnvelopeF::fromStages(z.ampEnv);
                z.modEnvPts = EnvelopeF::fromStages(z.modEnv);
                const int ch = z.stereo ? 2 : 1;
                if (z.numFrames > 0 && (size_t)z.numFrames * (size_t)ch == z.pcm->size())
                    zs.push_back(std::move(z));
            }
        }
        // envelopes: v3 stored Buzz {x,y,flags(int)}; v4 stores {x,y,sustain(u32)} the same width.
        EnvelopeF es[kNumEnvs];
        bool haveEnvs = false;
        if (ver >= 3) {
            uint32_t ne = g32();
            for (uint32_t e = 0; e < ne && at + 4 <= d.size(); ++e) {
                uint32_t np = g32();
                if (np > 8192) return;
                EnvelopeF env;
                for (uint32_t i = 0; i < np && at + 12 <= d.size(); ++i) {
                    EnvPointF pt;
                    if (ver >= 4) { pt.x = clamp01(gf()); pt.y = clamp01(gf()); pt.sustain = g32() != 0; }
                    else          { pt.x = (float)g32() / 65535.f; pt.y = (float)g32() / 65535.f; pt.sustain = (g32() & 1) != 0; }
                    if (pt.sustain) env.sustainIdx = (int)env.pts.size();
                    env.pts.push_back(pt);
                }
                env.enabled = !env.pts.empty();
                if (e < (uint32_t)kNumEnvs) es[e] = std::move(env);
            }
            haveEnvs = true;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        for (Voice& v : voices_) v = Voice{};       // silence -- pointers into old zones die
        zones_.swap(zs);
        rebuildZoneIndex();
        if (haveEnvs) for (int e = 0; e < kNumEnvs; ++e) envs_[e] = std::move(es[e]);
    }

    // ---- sample / zone / envelope loading (called from the free functions) ---
    //! Copying entry point: interns a private buffer for this one zone.  This is
    //! what the original sampler_load_sample[_ex] callers get, unchanged.
    void loadZone(int slot, int level, int rootKey, int loKey, int hiKey, int loVel, int hiVel,
                  bool noteOffLayer, bool keyToPitch, bool velToVol, int overlapMode,
                  const float* interleaved, int numFrames, bool stereo, int sampleRate,
                  int loopStart, int loopEnd, bool loop, const char* name) {
        if (!interleaved || numFrames <= 0) return;
        const size_t n = (size_t)numFrames * (size_t)(stereo ? 2 : 1);
        loadZoneShared(slot, level, rootKey, loKey, hiKey, loVel, hiVel,
                       noteOffLayer, keyToPitch, velToVol, overlapMode,
                       std::make_shared<const std::vector<float>>(interleaved, interleaved + n),
                       numFrames, stereo, sampleRate, loopStart, loopEnd, loop, name);
    }

    //! SHARING entry point.  The caller has already interned the buffer, so N
    //! zones over one sample cost ONE allocation.  Everything below this line is
    //! identical for both paths -- there is exactly one zone-installation
    //! routine, so the copying path can never drift from the sharing one.
    void loadZoneShared(int slot, int level, int rootKey, int loKey, int hiKey, int loVel, int hiVel,
                        bool noteOffLayer, bool keyToPitch, bool velToVol, int overlapMode,
                        SharedPcm pcm, int numFrames, bool stereo, int sampleRate,
                        int loopStart, int loopEnd, bool loop, const char* name) {
        if (!pcm || numFrames <= 0) return;
        // A buffer that does not match the frame count would let the cubic
        // reader walk off the end of somebody ELSE'S shared audio, so it is
        // rejected here rather than clamped.
        if (pcm->size() != (size_t)numFrames * (size_t)(stereo ? 2 : 1)) return;
        std::lock_guard<std::mutex> lk(mutex_);
        SampleZone z;
        z.slot = slot < 1 ? kDefaultSlot : slot; z.level = std::max(0, level);
        z.rootKey = rootKey; z.loKey = loKey; z.hiKey = hiKey; z.loVel = loVel; z.hiVel = hiVel;
        z.noteOffLayer = noteOffLayer; z.keyToPitch = keyToPitch; z.velToVol = velToVol;
        z.overlapMode = overlapMode; z.stereo = stereo; z.numFrames = numFrames;
        z.sampleRate = sampleRate > 0 ? sampleRate : (int)sr_;
        z.loopStart = std::max(0, loopStart); z.loopEnd = loopEnd > 0 ? loopEnd : numFrames;
        z.loop = loop; z.name = name ? name : "";
        z.pcm = std::move(pcm);
        sanitizeZone(z);
        // O(1) replace lookup: the old linear scan here made an N-zone import
        // O(N^2) in zone structs walked.
        auto found = zoneBySlotLevel_.find(slKey(z.slot, z.level));
        if (found != zoneBySlotLevel_.end()) {
            const int idx = (int)found->second;
            SampleZone& old = zones_[(size_t)idx];
            {
                // Swapping the PCM in a slot keeps that zone's per-zone
                // parameters: the envelope/filter/tuning belong to the KEYZONE,
                // not to whichever sample is currently sitting in it, so
                // auditioning a different sample must not silently wipe the
                // envelope the user just drew.  (An importer that means to set
                // them writes them straight after the load either way.)
                z.ampEnv = old.ampEnv; z.modEnv = old.modEnv;
                // ...and their drawable forms: keeping the stages but dropping
                // the pts made the envelope survive get_zone_env yet vanish
                // from PLAYBACK after a sample swap.
                z.ampEnvPts = old.ampEnvPts; z.modEnvPts = old.modEnvPts;
                z.cutoffHz = old.cutoffHz; z.resonanceDb = old.resonanceDb;
                z.coarseTune = old.coarseTune; z.fineTune = old.fineTune;
                z.scaleTuning = old.scaleTuning;
                z.pan = old.pan; z.attenuationDb = old.attenuationDb;
                z.exclusiveClass = old.exclusiveClass;
                z.modEnvToPitchCents = old.modEnvToPitchCents;
                z.modEnvToFilterCents = old.modEnvToFilterCents;
                // Only THIS zone's reference to the old buffer goes away.  Any
                // other zone still sharing that sample keeps it alive and keeps
                // playing it -- which is the whole point of a shared, immutable
                // buffer, and why swapping one velocity layer's sample cannot
                // disturb the seven layers beside it.
                killVoicesInSlot(z.slot);
                const int oldLo = old.loKey, oldHi = old.hiKey;
                old = std::move(z);
                // The zone kept its index, but a changed key range moves it
                // between key buckets.
                if (oldLo != old.loKey || oldHi != old.hiKey) {
                    unindexZoneKeys(idx, oldLo, oldHi);
                    indexZoneKeys(idx, old.loKey, old.hiKey);
                }
                return;
            }
        }
        // UNDEFINED BEHAVIOUR GUARD.  Every voice holds a raw `const SampleZone*`
        // INTO this vector, so a push_back that reallocates leaves every sounding
        // voice pointing at freed storage -- it then reads its PCM, numFrames,
        // sampleRate and loop points out of dead memory.  killVoicesInSlot() does
        // not cover it: the new zone is a DIFFERENT slot, so the voices left
        // dangling are precisely the ones it does not touch.  Loading a sample is
        // a message-thread editor action, so silencing everything is the cheap and
        // provably correct answer.
        killAllVoices();
        const int newIdx = (int)zones_.size();
        zoneBySlotLevel_[slKey(z.slot, z.level)] = newIdx;
        indexZoneKeys(newIdx, z.loKey, z.hiKey);
        zones_.push_back(std::move(z));
    }
    void clearSlot(int slot) {
        if (slot < 1) slot = kDefaultSlot;
        std::lock_guard<std::mutex> lk(mutex_);
        // erase() shifts every later element down, so voices pointing at zones
        // AFTER the erased one silently retarget to the wrong zone (or past the
        // end).  Same dangling-pointer class as loadZone() above.
        killAllVoices();
        const size_t before = zones_.size();
        zones_.erase(std::remove_if(zones_.begin(), zones_.end(),
                     [slot](const SampleZone& z){ return z.slot == slot; }), zones_.end());
        // erase moved every later zone's index; the rebuild is O(total key
        // span), so clearing MANY slots one call at a time is quadratic --
        // a full-patch replace must use clearAllZones() below instead.
        if (zones_.size() != before) rebuildZoneIndex();
    }
    //! Remove EVERY zone under ONE lock.  A full-patch replace (SF2 import)
    //! used to clear its ~2976 slots one clearSlot() at a time -- O(N^2) in
    //! SampleZone moves (~4.4 million for the measured Concert Grand, 458 ms
    //! per re-import), plus an O(N) index rebuild per call.  One call, one
    //! voice kill, one index clear.
    void clearAllZones() {
        std::lock_guard<std::mutex> lk(mutex_);
        killAllVoices();             // same dangling-voice-pointer guard as clearSlot
        zones_.clear();
        rebuildZoneIndex();
    }
    void setEnvelope(int env, const unsigned short* xs, const unsigned short* ys,
                     const int* flags, int count) {
        if (env < 0 || env >= kNumEnvs) return;
        std::lock_guard<std::mutex> lk(mutex_);
        EnvelopeF e;
        for (int i = 0; i < count; ++i) {
            EnvPointF pt;
            pt.x = clamp01((float)xs[i] / 65535.f);
            pt.y = clamp01((float)ys[i] / 65535.f);
            pt.sustain = flags && (flags[i] & (int)kSustainFlag);
            if (pt.sustain) e.sustainIdx = (int)e.pts.size();
            e.pts.push_back(pt);
        }
        e.enabled = !e.pts.empty();
        envs_[env] = std::move(e);
    }

    // ---- per-zone parameters (SF2 generator parity) -------------------------
    //! Find a zone by (slot, level).  Callers hold the lock.  Returns null when
    //! there is no such zone, so every setter below is a harmless no-op then.
    //! O(1) via the index: an SF2 import calls the seven setters once per zone
    //! (~21,000 calls for a 2976-zone preset), and the old linear scan made
    //! that walk ~31 million zone structs.
    SampleZone* zoneAt(int slot, int level) {
        auto it = zoneBySlotLevel_.find(slKey(slot, level));
        return it == zoneBySlotLevel_.end() ? nullptr : &zones_[(size_t)it->second];
    }
    const SampleZone* zoneAt(int slot, int level) const {
        auto it = zoneBySlotLevel_.find(slKey(slot, level));
        return it == zoneBySlotLevel_.end() ? nullptr : &zones_[(size_t)it->second];
    }
    //! Setting a per-zone parameter mutates a zone IN PLACE -- it never grows or
    //! shrinks `zones_` -- so the raw zone pointers sounding voices hold stay
    //! valid and no voice has to be killed.  Voices snapshot these at note-on,
    //! so the change lands on the NEXT note, not on notes already ringing.
    void setZoneEnv(int slot, int level, int env, const SamplerZoneEnv* e) {
        if (env < 0 || env > 1) return;
        std::lock_guard<std::mutex> lk(mutex_);
        SampleZone* z = zoneAt(slot, level);
        if (!z) return;
        SamplerZoneEnv v;                       // null / disabled == clear the
        if (e) v = *e;                          // override -> fall back again
        if (!e || !v.enabled) v = SamplerZoneEnv{};
        (env == 0 ? z->ampEnv : z->modEnv) = v;
        sanitizeZoneParams(*z);
        // Convert to the drawable form ONCE, here on the message thread, so the
        // audio thread only ever evaluates points -- never stages.
        (env == 0 ? z->ampEnvPts : z->modEnvPts) =
            EnvelopeF::fromStages(env == 0 ? z->ampEnv : z->modEnv);
    }
    bool getZoneEnv(int slot, int level, int env, SamplerZoneEnv* out) const {
        if (!out || env < 0 || env > 1) return false;
        std::lock_guard<std::mutex> lk(mutex_);
        if (const SampleZone* z = zoneAt(slot, level)) {
            *out = (env == 0 ? z->ampEnv : z->modEnv);
            return true;
        }
        return false;
    }
    void setZoneFilter(int slot, int level, float cutoffHz, float resonanceDb) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (SampleZone* z = zoneAt(slot, level)) {
            z->cutoffHz = cutoffHz; z->resonanceDb = resonanceDb;
            sanitizeZoneParams(*z);
        }
    }
    void setZoneTuning(int slot, int level, int coarse, int fine, int scaleTuning) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (SampleZone* z = zoneAt(slot, level)) {
            z->coarseTune = coarse; z->fineTune = fine; z->scaleTuning = scaleTuning;
            sanitizeZoneParams(*z);
        }
    }
    void setZoneLevel(int slot, int level, float pan, float attenuationDb) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (SampleZone* z = zoneAt(slot, level)) {
            z->pan = pan; z->attenuationDb = attenuationDb;
            sanitizeZoneParams(*z);
        }
    }
    void setZoneExclusive(int slot, int level, int exclusiveClass) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (SampleZone* z = zoneAt(slot, level)) {
            z->exclusiveClass = exclusiveClass;
            sanitizeZoneParams(*z);
        }
    }
    void setZoneModRoute(int slot, int level, float toPitchCents, float toFilterCents) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (SampleZone* z = zoneAt(slot, level)) {
            z->modEnvToPitchCents = toPitchCents; z->modEnvToFilterCents = toFilterCents;
            sanitizeZoneParams(*z);
        }
    }

    // ---- read-back for the editor to re-show state after a load -------------
    int  exportZoneCount() const { std::lock_guard<std::mutex> lk(mutex_); return (int)zones_.size(); }
    bool exportZone(int i, SamplerZoneInfo& z, bool includePcm=true) const {
        // The PCM copy happens OUTSIDE the lock: only the cheap shared_ptr
        // reference is taken while holding mutex_.  process() waits on this
        // same mutex, so copying megabytes of audio inside it stalled the
        // AUDIO thread for the duration of every full read-back (measured
        // 36 ms over a synthetic 36 MB bank; seconds on a real 500 MB one).
        SharedPcm pcmRef;
        {
        std::lock_guard<std::mutex> lk(mutex_);
        if (i < 0 || i >= (int)zones_.size()) return false;
        const SampleZone& s = zones_[(size_t)i];
        z.slot = s.slot; z.level = s.level; z.rootKey = s.rootKey;
        z.loKey = s.loKey; z.hiKey = s.hiKey; z.loVel = s.loVel; z.hiVel = s.hiVel;
        z.noteOffLayer = s.noteOffLayer; z.keyToPitch = s.keyToPitch; z.velToVol = s.velToVol;
        z.overlapMode = s.overlapMode; z.sampleRate = s.sampleRate;
        z.loopStart = s.loopStart; z.loopEnd = s.loopEnd; z.loop = s.loop;
        z.stereo = s.stereo; z.numFrames = s.numFrames; z.name = s.name;
        // The per-zone parameters ride the SAME read-back path the shell uses to
        // rebuild its editor state after a project load, so they survive a load
        // exactly as the key ranges and loop points do.
        z.ampEnv = s.ampEnv; z.modEnv = s.modEnv;
        z.cutoffHz = s.cutoffHz; z.resonanceDb = s.resonanceDb;
        z.coarseTune = s.coarseTune; z.fineTune = s.fineTune;
        z.scaleTuning = s.scaleTuning;
        z.pan = s.pan; z.attenuationDb = s.attenuationDb;
        z.exclusiveClass = s.exclusiveClass;
        z.modEnvToPitchCents = s.modEnvToPitchCents;
        z.modEnvToFilterCents = s.modEnvToFilterCents;
        // SamplerZoneInfo keeps a PLAIN vector, so sampler_get_zone() behaves
        // exactly as it always has (the caller gets its own copy).  The
        // _meta variant still never touches the PCM at all -- which is what
        // keeps rebuilding an editor over a multi-gigabyte bank cheap.
        if (includePcm) pcmRef = s.pcm;
        }
        // Buffer is immutable and kept alive by pcmRef; safe to copy unlocked.
        if (pcmRef) z.pcm = *pcmRef; else z.pcm.clear();
        return true;
    }
    //! The raw buffer a zone reads from.  Identity, so a caller can see that
    //! two zones really are one allocation.
    const float* zonePcmPtr(int i) const {
        std::lock_guard<std::mutex> lk(mutex_);
        if (i < 0 || i >= (int)zones_.size()) return nullptr;
        const SharedPcm& p = zones_[(size_t)i].pcm;
        return (p && !p->empty()) ? p->data() : nullptr;
    }
    bool hasEnvelopes() const {
        std::lock_guard<std::mutex> lk(mutex_);
        for (int e = 0; e < kNumEnvs; ++e) if (!envs_[e].pts.empty()) return true;
        return false;
    }

private:
    // ---- native FX params ---------------------------------------------------
    enum NativeParam {
        N_OUTPUT_GAIN, N_PAN, N_WIDTH, N_DRIVE, N_LP, N_HP, N_BITDEPTH,
        N_DOWNSAMPLE, N_TRANSPOSE, N_VELOCITY, N_KEY_LOW, N_KEY_HIGH,
        N_MONO, N_SWAP, N_NOISE, N_OFFSET,
        // Keep the original ids stable: sampler-playback controls are appended.
        // These are ordinary native parameters, therefore the tracker FX picker
        // and DAW automation system see exactly the same controls as the UI.
        N_GRANULAR, N_GRAIN_SIZE, N_GRAIN_DENSITY, N_GRAIN_SPRAY,
        N_GRAIN_JITTER, N_GRAIN_SHAPE, N_GRAIN_SPREAD, N_GRAIN_REVERSE,
        // Declick / offset-behaviour controls (appended -- ids above stay stable
        // so existing projects and FX-column assignments keep pointing at the
        // same parameters).
        N_DECLICK,       // voice-start / steal fade length, 0..5 ms (0 = off)
        N_ZERO_SNAP,     // snap the start offset to a zero crossing, 0 = off
        N_OFFSET_MODE,   // 0 = clean retrigger (default), 1 = Akai smear
        N_SMEAR_GRAIN,   // Akai grain length, 2..50 ms
        N_COUNT
    };
    static_assert(N_COUNT <= 32, "the per-voice override mask is a uint32_t");
    static const char* nativeName(int i) {
        static const char* n[N_COUNT] = {
            "Output Gain","Pan","Stereo Width","Drive","Lowpass","Highpass",
            "Bit Depth","Downsample","Transpose","Velocity","Key Low","Key High",
            "Mono","Stereo Swap","Noise","Sample Offset",
            "Granular","Grain Size","Grain Density","Grain Spray",
            "Grain Jitter","Grain Shape","Grain Spread","Reverse Grains",
            "Declick","Zero Snap","Offset Mode","Smear Grain" };
        return n[i];
    }
    static float nativeDefault(int i) {
        // N_DECLICK 0.4 == 2 ms: inside the 1-3 ms window every hardware sampler
        // uses, long enough to kill the step, short enough to stay percussive.
        // N_OFFSET_MODE 0 == the clean retrigger path: Akai smear is opt-in.
        static const float d[N_COUNT] = {
            0.5f,0.5f,0.5f,0.f,1.f,0.f,1.f,0.f,0.5f,1.f,0.f,1.f,0.f,0.f,0.f,0.f,
            0.f,0.3f,0.5f,0.f,0.f,0.5f,0.f,0.f,
            0.4f,0.f,0.f,0.2f };
        return d[i];
    }

    //! Declick/crossfade length in SAMPLES for a given N_DECLICK value.  Derived
    //! from the sample rate every time, never a baked-in sample count, so the
    //! fade is the same number of MILLISECONDS at 44.1k and at 192k.
    int declickSamples(float norm) const {
        const int n = (int)std::lround(5.0 * (double)clamp01(norm) * 0.001 * sr_);
        return n < 1 ? 0 : n;      // 0 == declick disabled (A/B against the old path)
    }
    //! Akai smear grain length in samples (2..50 ms), likewise sample-rate derived.
    int smearSamples(float norm) const {
        const int n = (int)std::lround((0.002 + 0.048 * (double)clamp01(norm)) * sr_);
        return n < 4 ? 4 : n;
    }

    // ---- one polyphonic voice ----------------------------------------------
    struct Voice {
        struct Grain {
            bool active = false;
            double pos = 0.0, rate = 1.0;
            int age = 0, life = 0, dir = 1;
            float panL = 1.f, panR = 1.f;
        };
        static constexpr int kMaxGrains = 24;
        bool  active = false;
        int   note = -1;               // ORIGINAL MIDI note (for note-off match)
        int   column = -1;             // tracker note-column that triggered this voice
        // RT-SAFETY / LIFETIME.  A voice holds a RAW pointer to its zone, and
        // reads that zone's SHARED PCM buffer through it.  The voice
        // deliberately does NOT hold a shared_ptr of its own: dropping the last
        // reference would then free a multi-megabyte buffer from inside
        // process(), which is a deallocation on the audio thread and exactly the
        // kind of unbounded work an RT callback must never do.
        //
        // Lifetime is guaranteed by the existing block-lock discipline instead:
        // process() holds mutex_ for the whole block, and every mutation of
        // zones_ (loadZone / clearSlot / loadState / the per-zone setters) takes
        // the same lock, so no buffer can be released while a voice is reading
        // it.  Mutations that MOVE zones_' elements additionally kill the
        // affected voices first, which is what makes the raw pointer safe.
        const SampleZone* zone = nullptr;
        double pos = 0.0;              // fractional frame position
        double step = 1.0;            // frames advanced per output sample (pitch)
        float velGain = 1.0f;
        // per-COLUMN FX modulation (neutral by default -> untagged voices render
        // exactly as before; a column-tagged FX write updates ONLY this voice).
        float  modGain  = 1.0f;        // per-column volume (0..1 linear)
        float  modPan   = 0.0f;        // per-column pan (-1..1), applied pre-tail
        double modPitch = 1.0;         // per-column pitch ratio (multiplies step)
        // The transposition ALREADY folded into v.step at note-on (via the
        // played note).  modPitch is measured against it, so a live column
        // transpose retunes the voice without applying the shift a second time.
        double pitchBaked = 1.0;
        // amp envelope
        bool  released = false;
        double envPhase = 0.0;        // 0..1 along the amp env
        double envPhaseMod = 0.0;     // 0..1 along the instrument's MOD env
        // Last-known envelope segment for evalHinted(): amortised O(1) lookup
        // instead of an O(points) scan per sample.  Plain ints, reset with the
        // voice; stale values are self-healing (see evalHinted).
        int    ampEnvHint = 0;
        int    modEnvHint = 0;
        float envLast = 0.f;
        double age = 0.0;             // for stealing (samples alive)
        Grain grains[kMaxGrains];
        double grainClock = 0.0;
        uint32_t grainRng = 0x9e3779b9u;

        // ---- declick / retrigger crossfade ---------------------------------
        // Every voice start ramps up over N_DECLICK ms, and a voice that is
        // retriggered or stolen is NOT cut: it is moved to a spare slot as a
        // `fadingOut` tail that ramps down over the same window, so the two
        // overlap into a constant-gain crossfade.
        float fadePhase = 1.f;        // 0..1 along the declick ramp
        float fadeRate  = 0.f;        // per-sample delta: >0 in, <0 out
        bool  fadingOut = false;      // a tail: never matched by note-off/column

        // ---- Akai smear (N_OFFSET_MODE == 1) -------------------------------
        // Two overlapping Hann-windowed grains that MIGRATE the read pointer
        // toward a new offset instead of jumping it, which is what gives the
        // S-series its time-compress/expand smear.
        double smearPos[2]  = { 0.0, 0.0 };
        int    smearAge[2]  = { 0, 0 };
        bool   smearOn[2]   = { false, false };
        int    smearLen     = 0;      // grain length in frames
        int    smearNext    = 0;      // samples until the next grain launch
        double smearShift   = 0.0;    // frames of offset travel already consumed
        double baseOffset   = 0.0;    // start offset this voice was launched at
        double targetOffset = 0.0;    // offset the parameter currently asks for
        float  offsetSeen   = -1.f;   // last N_OFFSET this voice retargeted on

        // ---- per-column FX isolation ---------------------------------------
        // A voice owns a COPY of every automatable parameter value plus ALL of
        // the DSP state those parameters drive.  That is the whole point: making
        // the parameter per-column but leaving the decimator phase, the filter
        // state or the read pointer shared would still let one tracker column
        // chop another column's notes on its clock.  `fxMask` marks which ids
        // this voice's column overrides -- only those stages run per-voice, the
        // rest are left to the global bus chain exactly as before.
        float    fx[N_COUNT] = { 0.f };
        uint32_t fxMask = 0u;
        float    fxLpL = 0.f, fxLpR = 0.f, fxHpL = 0.f, fxHpR = 0.f;
        int      fxDecPhase = 0;                  // decimator clock (per voice!)
        float    fxHeldL = 0.f, fxHeldR = 0.f;    // decimator sample-and-hold
        uint32_t fxRng = 0x2545f491u;             // per-voice noise generator

        // ---- per-ZONE parameters, SNAPSHOT AT NOTE-ON ----------------------
        // The zone supplies the SHAPE; the running state is the voice's, which
        // is what makes a per-zone release actually per-note.  A zone with no
        // envelope of its own leaves `on == false` and the render path uses the
        // instrument-global envelope exactly as it always has.
        //! Which drawable envelope this note plays: the zone's own when it has
        //! one, otherwise null meaning "use the instrument's".  Chosen once at
        //! note-on, so a later edit cannot change an already-sounding note's
        //! shape underneath it.
        const EnvelopeF* ampEnvPts = nullptr;
        const EnvelopeF* modEnvPts = nullptr;
        //! Last amp-envelope level, held while a declick TAIL finishes.
        float            lastEnvAmp = 1.f;
        int    exclusiveClass = 0;    // choke group this voice belongs to
        float  zonePan   = 0.f;       // -1..+1, summed with the column's pan
        float  zoneGain  = 1.f;       // from attenuationDb
        float  cutoffHz  = 0.f;       // 0 = per-zone filter off
        float  filtQ     = 0.70710678f;
        float  modToPitchCents = 0.f, modToFilterCents = 0.f;
        // TPT state-variable filter: state is PER VOICE, so one note's filter
        // ring can never be excited by another note's audio.
        float  svfIc1L = 0.f, svfIc2L = 0.f, svfIc1R = 0.f, svfIc2R = 0.f;
        float  svfCachedHz = -1.f;    // cutoff the coefficients below were built for
        float  svfA1 = 0.f, svfA2 = 0.f, svfA3 = 0.f;
    };

    void handleMidi(const MidiEvent& m) {
        const unsigned char st = m.status & 0xF0u;
        // The tracker COLUMN rides on the event.  The legacy pitch-keyed table is
        // consulted only for UNTAGGED events (live MIDI in, UI preview, imported
        // files) -- it cannot distinguish two columns playing the same pitch, so
        // it must never override a column the event actually carries.
        const int col = (m.column >= 0)
                          ? (int)m.column
                          : ((m.data1 < 128) ? (int)noteColumn_[m.data1] : -1);
        if (st == 0x90u && m.data2 > 0) noteOn(m.data1, m.data2, col);
        else if (st == 0x80u || (st == 0x90u && m.data2 == 0)) noteOff(m.data1, col);
    }

    //! ---- zone lookup index ------------------------------------------------
    //! With a real multisampled bank loaded (a Concert Grand measures 2976
    //! zones) the old per-note-on linear scan cost ~5 us per key press ON THE
    //! AUDIO THREAD, and the per-(slot,level) setter scan made an import walk
    //! O(zones^2) structs.  Both lookups now go through an index:
    //!   * keyZones_[k] -- the zone indices whose [loKey,hiKey] covers key k,
    //!     kept in zone order so the nearest-root tie-break picks the SAME zone
    //!     the full scan picked;
    //!   * zoneBySlotLevel_ -- (slot,level) -> zone index for the setters and
    //!     the load-replace path.
    //! The index is built and mutated ONLY on the message thread, under mutex_.
    //! The audio thread (which already holds mutex_ for the whole of process())
    //! only ever READS it, so the audio path gains no allocation and no new
    //! locking.  Zone indices are stable except across erase/swap, where the
    //! whole index is rebuilt.
    static uint64_t slKey(int slot, int level) {
        return ((uint64_t)(uint32_t)slot << 32) | (uint32_t)level;
    }
    void indexZoneKeys(int idx, int lo, int hi) {
        lo = std::max(0, std::min(127, lo)); hi = std::max(0, std::min(127, hi));
        for (int k = lo; k <= hi; ++k) {
            std::vector<int32_t>& b = keyZones_[k];
            b.insert(std::lower_bound(b.begin(), b.end(), idx), (int32_t)idx);
        }
    }
    void unindexZoneKeys(int idx, int lo, int hi) {
        lo = std::max(0, std::min(127, lo)); hi = std::max(0, std::min(127, hi));
        for (int k = lo; k <= hi; ++k) {
            std::vector<int32_t>& b = keyZones_[k];
            auto it = std::lower_bound(b.begin(), b.end(), idx);
            if (it != b.end() && *it == idx) b.erase(it);
        }
    }
    void rebuildZoneIndex() {
        for (auto& b : keyZones_) b.clear();
        zoneBySlotLevel_.clear();
        zoneBySlotLevel_.reserve(zones_.size());
        for (int i = 0; i < (int)zones_.size(); ++i) {
            const SampleZone& z = zones_[(size_t)i];
            indexZoneKeys(i, z.loKey, z.hiKey);
            // emplace, not operator[]: on a (malformed) blob carrying duplicate
            // (slot,level) pairs the old linear scan answered with the FIRST
            // match, so the index must too.
            zoneBySlotLevel_.emplace(slKey(z.slot, z.level), (int32_t)i);
        }
    }

    const SampleZone* zoneForMidi(int midiNote, int vel) const {
        if (midiNote < 0 || midiNote > 127) return nullptr;
        const SampleZone* best = nullptr; int bestDist = 1 << 30;
        // Only the zones whose key range covers this note, in zone order --
        // the same candidates the full scan tested, minus the ~97% that could
        // never match, so best/tie-break behaviour is IDENTICAL.
        for (int32_t idx : keyZones_[midiNote]) {
            const SampleZone& z = zones_[(size_t)idx];
            if (z.noteOffLayer) continue;
            if (vel < z.loVel || vel > z.hiVel) continue;
            int d = std::abs(midiNote - z.rootKey);
            if (d < bestDist) { bestDist = d; best = &z; }
        }
        return best;
    }

    //! How loud a voice is RIGHT NOW -- the input to voice stealing, and the
    //! reason stealing no longer just grabs slot 0 or the oldest voice.
    float voiceLevel(const Voice& v) const {
        if (!v.active) return 0.f;
        float a = v.velGain * v.modGain * v.zoneGain * declickShape(v.fadePhase);
        // Same disabled-falls-back rule as renderVoices, so steal scoring sees
        // the gain the voice is actually rendered with.  Evaluated through the
        // voice's own segment hint (a LOCAL copy -- v is const here): stealVoice
        // calls this for all 32 voices per steal, and with a dense hand-drawn
        // envelope the O(points) eval() made every steal cost ~6.8 us on the
        // audio thread.  The hint the render loop maintains lands on the same
        // segment, so the value is bit-identical.
        const EnvelopeF& e = (v.ampEnvPts && v.ampEnvPts->enabled) ? *v.ampEnvPts : envs_[0];
        if (e.enabled) { int h = v.ampEnvHint; a *= e.evalHinted(v.envPhase, h); }
        return a > 0.f ? a : 0.f;
    }

    //! Steal the voice that will be missed least: the QUIETEST one, with age as
    //! the tie-break -- not slot 0, and not merely the oldest, which at high note
    //! rates is regularly the one still at full level.
    int stealVoice() const {
        int best = -1; double bestScore = 1e300;
        for (int i = 0; i < kMaxVoices; ++i) {
            const Voice& v = voices_[i];
            if (!v.active) return i;
            double score = (double)voiceLevel(v);
            if (v.released) score *= 0.25;         // a released voice is cheaper
            score -= v.age * 1e-9;                 // older loses ties
            if (score < bestScore) { bestScore = score; best = i; }
        }
        return best < 0 ? 0 : best;
    }

    //! A slot from the reserved tail pool: free one first, else the quietest tail
    //! already in flight.
    int allocTailSlot() const {
        int best = -1; float bestLevel = 1e30f;
        for (int i = kMaxVoices; i < kTotalVoices; ++i) {
            if (!voices_[i].active) return i;
            const float lvl = voiceLevel(voices_[i]);
            if (lvl < bestLevel) { bestLevel = lvl; best = i; }
        }
        return best;
    }

    int allocVoice(int column) {
        // Per-column mono: a new note in a TAGGED column reuses that column's own
        // voice (retrigger in-column) so a retrigger never spawns a stray voice and
        // columns never bleed into one another.  Untagged (column<0) keeps the
        // original per-note free/steal behavior.  Tails are skipped -- they are
        // no longer anybody's voice, just audio finishing its fade.
        if (column >= 0) {
            int inCol = -1; double colAge = -1.0;
            for (int i = 0; i < kMaxVoices; ++i)
                if (voices_[i].active && !voices_[i].fadingOut &&
                    voices_[i].column == column && voices_[i].age > colAge)
                    { colAge = voices_[i].age; inCol = i; }
            if (inCol >= 0) return inCol;
        }
        for (int i = 0; i < kMaxVoices; ++i) if (!voices_[i].active) return i;
        return stealVoice();
    }

    //! Retriggering or stealing a voice must never CUT it -- that step from its
    //! last sample straight to the next voice's first sample is exactly the click
    //! being chased.  Copy it into a spare slot that ramps down over the same
    //! window the incoming voice ramps up, so the pair crossfades.
    void retireVoice(int vi, int fadeN) {
        Voice& src = voices_[(size_t)vi];
        if (!src.active || !src.zone || fadeN <= 0) return;
        if (src.fadingOut && src.fadePhase <= 0.f) return;
        const int ti = allocTailSlot();
        if (ti < 0 || ti == vi) return;          // nowhere to put it: hard cut
        Voice& t = voices_[(size_t)ti];
        t = src;
        t.fadingOut = true;
        t.note      = -1;      // never matched by a note-off
        t.column    = -1;      // never reused by a column retrigger
        t.released  = true;    // do not hold at the envelope sustain point
        if (t.fadePhase > 1.f) t.fadePhase = 1.f;
        t.fadeRate  = -1.f / (float)fadeN;
    }

    //! Snap a start offset to the nearest zero crossing within +-`window` frames,
    //! which removes the step at the source instead of masking it.  OPTIONAL and
    //! off by default precisely because it MOVES the start by up to that window,
    //! and for tightly-timed material that shift is itself the artefact.
    static double snapToZeroCrossing(const SampleZone& z, double pos, int window) {
        if (window <= 0 || z.numFrames < 4) return pos;
        const int c = (int)std::lround(pos);
        const int lo = std::max(1, c - window);
        const int hi = std::min(z.numFrames - 2, c + window);
        if (hi <= lo) return pos;
        const float* p = z.pcm->data();
        const int st = z.stereo ? 2 : 1;
        auto mono = [&](int i) -> float {
            return z.stereo ? 0.5f * (p[(size_t)i * 2] + p[(size_t)i * 2 + 1])
                            : p[(size_t)i * (size_t)st];
        };
        int best = -1, bestD = 1 << 30;
        float prev = mono(lo - 1);
        for (int i = lo; i <= hi; ++i) {
            const float cur = mono(i);
            if ((prev <= 0.f && cur >= 0.f) || (prev >= 0.f && cur <= 0.f)) {
                const int d = std::abs(i - c);
                if (d < bestD) { bestD = d; best = i; }
            }
            prev = cur;
        }
        return best >= 0 ? (double)best : pos;
    }

    //! The value of parameter `id` as seen by note-column `col`: the column's own
    //! value when that column overrides it, otherwise the instrument-global one.
    float colVal(int col, int id) const {
        if (col >= 0 && col < kMaxColumns && (colSet_[col] & (1u << (unsigned)id)))
            return colFx_[col][id];
        return native_[id];
    }
    //! Give a voice its OWN copy of every parameter value, plus the mask of the
    //! ones its column owns.  From here the voice is self-contained for life.
    void seedVoiceFx(Voice& v, int col) const {
        const uint32_t mask = (col >= 0 && col < kMaxColumns) ? colSet_[col] : 0u;
        v.fxMask = mask;
        for (int i = 0; i < N_COUNT; ++i)
            v.fx[i] = (mask & (1u << (unsigned)i)) ? colFx_[col][i] : native_[i];
    }
    //! Copy the zone's own parameters into the voice at note-on.  From here the
    //! voice is self-contained: editing the zone later cannot retune or
    //! re-envelope a note that is already sounding, and two voices on two
    //! different zones run two genuinely independent envelopes.
    static void seedVoiceZone(Voice& v, const SampleZone& z) {
        v.ampEnvPts = z.ampEnvPts.enabled ? &z.ampEnvPts : nullptr;
        v.modEnvPts = z.modEnvPts.enabled ? &z.modEnvPts : nullptr;
        v.exclusiveClass = z.exclusiveClass;
        v.zonePan  = z.pan < -1.f ? -1.f : (z.pan > 1.f ? 1.f : z.pan);
        v.zoneGain = z.attenuationDb != 0.f ? db_to_gain(-z.attenuationDb) : 1.f;
        v.cutoffHz = z.cutoffHz > 0.f ? z.cutoffHz : 0.f;
        // SF2 initialFilterQ is a dB peak gain; Q = 10^(dB/20), floored at the
        // Butterworth value so "0 dB resonance" is a clean, non-peaking LP.
        v.filtQ    = std::max(0.7071068f, db_to_gain(z.resonanceDb));
        v.modToPitchCents  = z.modEnvToPitchCents;
        v.modToFilterCents = z.modEnvToFilterCents;
        v.svfIc1L = v.svfIc2L = v.svfIc1R = v.svfIc2R = 0.f;
        v.svfCachedHz = -1.f;
    }

    //! Cut a voice because something choked it (exclusive class).  It is FADED,
    //! not killed: a hard cut of a sounding voice is the same click the declick
    //! work exists to remove.  Detached from its note/column first so a later
    //! note-off or column retrigger can never resurrect it.
    void chokeVoice(Voice& v, int fadeN) {
        v.note = -1;
        v.column = -1;
        v.released = true;
        if (fadeN > 0) {
            if (v.fadePhase > 1.f) v.fadePhase = 1.f;
            v.fadingOut = true;
            v.fadeRate  = -1.f / (float)fadeN;
        } else {
            v.active = false;
        }
    }

    //! Pick up parameter writes that arrived globally (host automation via
    //! paramIn, or the non-RT setter from the UI) and push them into every voice
    //! that does NOT have a column-local override for that id.  A column-owned
    //! value has to survive a global write, otherwise the isolation would only
    //! hold until the next automation point on some other lane.
    void syncGlobalParams() {
        for (int id = 0; id < N_COUNT; ++id) {
            if (native_[id] == nativeSeen_[id]) continue;
            nativeSeen_[id] = native_[id];
            const uint32_t bit = 1u << (unsigned)id;
            for (Voice& v : voices_) {
                if (!v.active || (v.fxMask & bit)) continue;
                v.fx[id] = native_[id];
            }
        }
        // Akai mode: an offset change RETARGETS a sounding voice -- it travels
        // there through the grain engine instead of jumping its read pointer.
        for (Voice& v : voices_) {
            if (!v.active || v.fadingOut || !v.zone) continue;
            if (v.fx[N_OFFSET_MODE] < 0.5f || v.fx[N_OFFSET] == v.offsetSeen) continue;
            v.offsetSeen = v.fx[N_OFFSET];
            v.targetOffset = startOffset(*v.zone, v);
        }
    }

    //! Where in the sample this note should start, honouring the (optional)
    //! zero-crossing snap.
    double startOffset(const SampleZone& z, const Voice& v) const {
        const double last = (double)std::max(0, z.numFrames - 1);
        double p = (double)clamp01(v.fx[N_OFFSET]) * last;
        const int win = (int)std::lround(5.0 * (double)clamp01(v.fx[N_ZERO_SNAP]) * 0.001 * sr_);
        if (win > 0) p = snapToZeroCrossing(z, p, win);
        return p < 0.0 ? 0.0 : (p > last ? last : p);
    }

    void noteOn(int midiNote, int vel, int col) {
        const int low  = (int)std::lround(native_[N_KEY_LOW]  * 127.f);
        const int high = (int)std::lround(native_[N_KEY_HIGH] * 127.f);
        const int lo = std::min(low, high), hi = std::max(low, high);
        if (midiNote < lo || midiNote > hi) return;

        const int tr = (int)std::lround((colVal(col, N_TRANSPOSE) - 0.5f) * 48.f);
        const int playNote = std::max(0, std::min(127, midiNote + tr));
        const SampleZone* zone = zoneForMidi(playNote, vel);
        if (!zone) return;

        int useVel = zone->velToVol ? vel : 127;
        useVel = std::max(1, std::min(127, (int)std::lround(useVel * native_[N_VELOCITY])));

        const int   fadeN = declickSamples(colVal(col, N_DECLICK));
        const bool  akai  = colVal(col, N_OFFSET_MODE) >= 0.5f;

        int vi = allocVoice(col);
        Voice& v = voices_[(size_t)vi];

        // Akai mode holds the read pointer ACROSS retriggers: the note restarts
        // the envelope, but the offset change is travelled to (time-compressed /
        // expanded by the grain engine) rather than jumped to.
        const bool reuse = akai && v.active && !v.fadingOut && v.zone == zone &&
                           col >= 0 && v.column == col;
        if (!reuse) {
            retireVoice(vi, fadeN);        // crossfade the outgoing voice out
            v = Voice{};
            v.zone = zone;
            v.smearLen = smearSamples(colVal(col, N_SMEAR_GRAIN));
        }
        // EXCLUSIVE CLASS (SF2 `exclusiveClass`, the drum choke group).  A note
        // in a zone with a non-zero class cuts every OTHER sounding voice of the
        // same class on this instrument -- that is how a closed hi-hat kills an
        // open one.  Voice `vi` is skipped because the allocator has already
        // decided what happens to it (reuse or crossfaded retire), and tails are
        // skipped because they are already fading and own no note any more.
        if (zone->exclusiveClass != 0) {
            for (int i = 0; i < kTotalVoices; ++i) {
                if (i == vi) continue;
                Voice& o = voices_[(size_t)i];
                if (!o.active || o.fadingOut) continue;
                if (o.exclusiveClass != zone->exclusiveClass) continue;
                chokeVoice(o, fadeN);
            }
        }

        v.active = true;
        v.fadingOut = false;
        v.note = midiNote;                 // key by original note for note-off
        v.column = col;
        seedVoiceFx(v, col);               // the voice's own parameter copy
        if (col >= 0 && col < kMaxColumns) {   // seed the per-column FX modulation
            v.modGain = colGain_[col]; v.modPan = colPan_[col];
        }
        seedVoiceZone(v, *zone);           // per-zone envelopes / filter / level
        const int triggerNote = zone->keyToPitch ? playNote : zone->rootKey;
        // SF2 tuning: the key's distance from the root is scaled by scaleTuning
        // (100 = normal, 0 = fixed pitch for drum zones), then coarseTune
        // semitones and fineTune cents are added.  At the defaults
        // (100/0/0) this is exactly `triggerNote - rootKey` as before.
        const double scale = (double)zone->scaleTuning / 100.0;
        const double semis = (double)(triggerNote - zone->rootKey) * scale
                           + (double)zone->coarseTune + (double)zone->fineTune / 100.0;
        v.step = semisToRatio(semis) * ((double)zone->sampleRate / sr_);
        // TRANSPOSE IS APPLIED EXACTLY ONCE.  `tr` above (the column's value if
        // the column owns Transpose, else the global one) is already inside
        // playNote and therefore inside v.step; assigning colPitch_ to modPitch
        // as well multiplied it in a SECOND time, so an octave-up FX column
        // played two octaves up.  Record what got baked in and let modPitch
        // carry only the difference from it.
        v.pitchBaked = zone->keyToPitch
                         ? semisToRatio((double)(playNote - midiNote) * scale) : 1.0;
        const bool colOwnsTranspose = col >= 0 && col < kMaxColumns &&
                                      (colSet_[col] & (1u << (unsigned)N_TRANSPOSE)) != 0u;
        v.modPitch = (colOwnsTranspose && zone->keyToPitch)
                       ? colPitch_[col] / v.pitchBaked
                       : 1.0;
        v.velGain = (float)useVel / 127.f;
        v.released = false;
        v.envPhase = 0.0;
        v.age = 0.0;

        // Sample Offset (automatable, per-voice): start playback this far into the
        // sample.  Read at note-on so it's captured per-voice, not applied globally.
        const double want = startOffset(*zone, v);
        v.offsetSeen = v.fx[N_OFFSET];
        if (reuse) {
            v.targetOffset = want;         // smear there; do NOT move v.pos
        } else {
            v.pos = want;
            v.baseOffset = want;
            v.targetOffset = want;
            v.smearShift = 0.0;
            // Every voice start ramps up from silence over N_DECLICK ms.  Without
            // this the very first sample read at an arbitrary offset is a step
            // from 0 to whatever the waveform happens to be at that point.
            v.fadePhase = fadeN > 0 ? 0.f : 1.f;
            v.fadeRate  = fadeN > 0 ? 1.f / (float)fadeN : 0.f;
        }
    }

    void noteOff(int midiNote, int column) {
        for (Voice& v : voices_)
            if (v.active && !v.fadingOut && v.note == midiNote && !v.released) {
                // A TAGGED note-off releases only its own column's voice.
                // Matching on pitch alone released whichever voice was found
                // first, so with one pitch sounding in two columns the wrong
                // note got cut and the right one hung until the next panic.
                // An UNTAGGED off still matches any column -- imported MIDI,
                // piano-roll edits and panics have no other way to release.
                if (column >= 0 && v.column != column) continue;
                v.released = true;
                // A zone with its OWN amp envelope runs its own release from
                // here, at ITS release time and from ITS current level -- the
                // whole point of the per-zone envelope.  Nothing else to do:
                // the state is inside this voice, so the release cannot reach
                // any other voice, in this column or any other.
                // Jump the amp-env phase to (just past) the sustain point so the
                // release segment plays from here -- for WHICHEVER envelope this
                // voice plays.  Zone and instrument envelopes are the same kind
                // of object now, so there is one rule instead of two.
                // `enabled` is checked, not just the pointer: clearing a zone's
                // envelope while its note sounds leaves v.ampEnvPts aimed at a
                // now-DISABLED envelope.  Treating that as "has an envelope"
                // sent the voice down the envelope-release path with no
                // envelope to finish it -- a released looped voice then ran
                // FOREVER at full gain (burning a polyphony slot and its full
                // per-sample render cost until something happened to steal it).
                // Disabled falls through to the declick-fade release instead.
                const EnvelopeF* relEnv = (v.ampEnvPts && v.ampEnvPts->enabled)
                                        ? v.ampEnvPts
                                        : (envs_[0].enabled ? &envs_[0] : nullptr);
                if (relEnv) {
                    const double sx = relEnv->sustainX();
                    if (v.envPhase < sx) v.envPhase = sx;
                } else if (v.zone && v.zone->loop) {
                    // No amp envelope: `released` on its own changed NOTHING
                    // audible -- amp stayed at 1.0 and the only effect was to
                    // stop looping, so a sustained loop then ran on to the end
                    // of the file. Note length did nothing and voices piled up
                    // until the allocator started stealing mid-note.
                    // Give it a real release: the declick-length fade.
                    // One-shots are deliberately left to play out, which is what
                    // every hardware sampler does with an un-enveloped hit.
                    const int fadeN = declickSamples(v.fx[N_DECLICK]);
                    if (fadeN > 0) { v.fadingOut = true; v.fadeRate = -1.f / (float)fadeN; }
                    else           { v.active = false; }
                }
            }
    }

    void killVoicesInSlot(int slot) {
        for (Voice& v : voices_) if (v.active && v.zone && v.zone->slot == slot) v = Voice{};
    }

    //! Silence every voice AND drop its zone pointer.  Call before any mutation
    //! of `zones_` that can move its elements (push_back / erase): voices hold
    //! raw pointers into that vector, so a reallocation turns them into reads of
    //! freed memory.  Covers the reserved declick tails too -- a tail is still a
    //! voice rendering from a zone.
    void killAllVoices() {
        for (Voice& v : voices_) v = Voice{};
    }

    //! Akai-style smear: instead of jumping the read pointer to a new offset,
    //! two overlapping windowed grains WALK it there, consuming at most one hop
    //! of travel per grain, which is heard as the S-series' time compress/expand
    //! rather than as a retrigger.  Deliberately grains-and-crossfade, not a
    //! phase vocoder -- the smear IS the sound.
    void renderSmear(Voice& v, const SampleZone& z, float& outL, float& outR) {
        if (v.smearLen < 4) v.smearLen = smearSamples(v.fx[N_SMEAR_GRAIN]);
        v.smearLen &= ~1;                               // even: hop*2 == smearLen
        const int hop = v.smearLen / 2;                 // 50% overlap -> COLA
        const double last = (double)z.numFrames - 1.0;
        // The window a grain is allowed to roam in.  A grain must NEVER be cut
        // short for running past it -- killing a grain mid-window is itself a
        // step, which is the very thing this mode exists to avoid -- so positions
        // wrap (looped zones) or simply hold at the edge (one-shots).
        const double lo   = z.loop ? (double)z.loopStart : 0.0;
        const double le   = z.loop ? (z.loopEnd > z.loopStart ? (double)z.loopEnd : last) : last;
        const double span = le - lo;
        auto contain = [&](double p) {
            if (z.loop && span > 1.0) {
                while (p >= le) p -= span;
                while (p < lo)  p += span;
                return p;
            }
            return p < 0.0 ? 0.0 : (p > last ? last : p);
        };
        if (--v.smearNext <= 0) {
            v.smearNext = hop;
            // launch into the free slot, else over the older of the two (which is
            // at the tail of its window, so its contribution is ~0 anyway)
            const int gi = !v.smearOn[0] ? 0 : (!v.smearOn[1] ? 1
                                        : (v.smearAge[0] >= v.smearAge[1] ? 0 : 1));
            // Travel toward the requested offset, capped at one hop per grain --
            // that cap is what bounds the stretch to ~2x compress/expand and is
            // why this reads as time-stretch rather than as a jump.
            double delta = (v.targetOffset - v.baseOffset) - v.smearShift;
            if (delta >  (double)hop) delta =  (double)hop;
            if (delta < -(double)hop) delta = -(double)hop;
            v.smearShift += delta;
            v.smearPos[gi] = contain(v.pos + v.smearShift);
            v.smearAge[gi] = 0;
            v.smearOn[gi]  = true;
        }
        outL = outR = 0.f;
        for (int gi = 0; gi < 2; ++gi) {
            if (!v.smearOn[gi]) continue;
            if (v.smearAge[gi] >= v.smearLen) { v.smearOn[gi] = false; continue; }
            float gl, gr; z.frame(v.smearPos[gi], gl, gr);
            const float w = grainWindow((float)v.smearAge[gi] / (float)v.smearLen);
            outL += gl * w; outR += gr * w;
            v.smearPos[gi] = contain(v.smearPos[gi] + v.step * v.modPitch);
            ++v.smearAge[gi];
        }
    }

    //! The FX stages this voice's COLUMN overrides, run on the voice's own audio
    //! with the voice's OWN state.  Stages the column does not override are left
    //! untouched here and handled once, globally, by applyNativeFx().
    //!
    //! Downsample is the case that makes this necessary rather than tidy: two
    //! columns sharing one decimator phase counter means column 2's notes get
    //! chopped on column 1's clock even when the parameter values are perfectly
    //! per-column.  Hence fxDecPhase/fxHeld* live on the voice.
    void applyVoiceFx(Voice& v, float& l, float& r) {
        const uint32_t m = v.fxMask;
        if (!m) return;
        auto has = [m](int id) { return (m & (1u << (unsigned)id)) != 0u; };
        if (has(N_SWAP) && v.fx[N_SWAP] > 0.5f) std::swap(l, r);
        if (has(N_WIDTH) || has(N_MONO)) {
            const float width = v.fx[N_WIDTH] * 2.f;
            const bool mono = v.fx[N_MONO] > 0.5f;
            const float mid = (l + r) * 0.5f, side = (l - r) * 0.5f * width;
            l = mono ? mid : mid + side;
            r = mono ? mid : mid - side;
        }
        if (has(N_LP)) {
            const float a = onepole_alpha(80.f + v.fx[N_LP] * v.fx[N_LP] * 19920.f, sr_);
            v.fxLpL += a * (l - v.fxLpL); v.fxLpR += a * (r - v.fxLpR);
            l = v.fxLpL; r = v.fxLpR;
        }
        if (has(N_HP)) {
            const float hz = v.fx[N_HP] * v.fx[N_HP] * 4000.f;
            if (hz > 1.f) {
                const float a = onepole_alpha(std::max(10.f, hz), sr_);
                v.fxHpL += a * (l - v.fxHpL); v.fxHpR += a * (r - v.fxHpR);
                l -= v.fxHpL; r -= v.fxHpR;
            }
        }
        if (has(N_DOWNSAMPLE)) {
            const int hold = 1 + (int)std::lround(v.fx[N_DOWNSAMPLE] * 31.f);
            if (v.fxDecPhase <= 0) { v.fxHeldL = l; v.fxHeldR = r; v.fxDecPhase = hold; }
            --v.fxDecPhase;
            l = v.fxHeldL; r = v.fxHeldR;
        }
        if (has(N_BITDEPTH)) {
            const float steps = std::pow(2.f, 4.f + v.fx[N_BITDEPTH] * 12.f);
            l = std::round(l * steps) / steps; r = std::round(r * steps) / steps;
        }
        if (has(N_NOISE)) {
            v.fxRng = v.fxRng * 1664525u + 1013904223u;
            const float nz = ((int)((v.fxRng >> 8) & 0xffff) / 32768.f - 1.f)
                           * v.fx[N_NOISE] * 0.02f;
            l += nz; r += nz;
        }
        // Drive at its 0.0 default must be a BYPASS.  tanh(x*1)/tanh(1) is not
        // unity -- it is ~1.31x with waveshaping -- so the "off" position was
        // quietly boosting and distorting every voice.
        if (has(N_DRIVE) && v.fx[N_DRIVE] > 0.f) {
            const float d = 1.f + v.fx[N_DRIVE] * 15.f;
            const float k = std::tanh(d);
            l = std::tanh(l * d) / k; r = std::tanh(r * d) / k;
        }
    }

    // Render all active voices additively into L/R for `n` samples from `off`.
    void renderVoices(float* L, float* R, int off, int n) {
        if (n <= 0) return;
        const float ampScale = 1.0f;
        const double envInc = 1.0 / (double)(kEnvSeconds * sr_);   // phase per sample
        for (Voice& v : voices_) {
            if (!v.active || !v.zone) continue;
            const SampleZone& z = *v.zone;
            // ENVELOPE SOURCE, picked per voice: the zone's own shape when it
            // has one, otherwise the instrument-global envelope.  The phase is
            // the voice's either way, which is why per-zone envelopes and the
            // existing per-column voice allocation compose without interfering.
            // A zone envelope DISABLED while this voice sounds (editor cleared
            // it mid-note) must fall back to the instrument envelope exactly as
            // a null pointer does -- following the pointer alone let a released
            // looped voice with a freshly-disabled envelope loop forever.
            const EnvelopeF* zoneAmp = (v.ampEnvPts && v.ampEnvPts->enabled)
                                     ? v.ampEnvPts : nullptr;
            const EnvelopeF& ampEnv = zoneAmp ? *zoneAmp : envs_[0];
            const bool useAmp = ampEnv.enabled;
            const double sustainX = ampEnv.sustainX();
            // The phase axis is normalised, but its DURATION is per envelope --
            // that is what lets one drawable envelope cover both a 2-second
            // hand-drawn shape and a piano's 100-second decay.
            const double ampInc = 1.0 / (double)(std::max(1.0e-3f, ampEnv.spanSec) * sr_);
            // The mod envelope only costs anything when something is routed
            // from it, so a zone with the (default) zero routes is bit-identical
            // to the old render path.
            const EnvelopeF& modEnvG = v.modEnvPts ? *v.modEnvPts : envs_[1];
            const bool modRouted = (v.modToPitchCents != 0.f || v.modToFilterCents != 0.f);
            const bool useModG    = modRouted && modEnvG.enabled;
            const double modSustainX = modEnvG.sustainX();
            const double modInc = 1.0 / (double)(std::max(1.0e-3f, modEnvG.spanSec) * sr_);
            const bool useFilter = v.cutoffHz > 0.f;
            const double dt = 1.0 / sr_;
            const bool granular = v.fx[N_GRANULAR] >= 0.5f;
            const bool akai = v.fx[N_OFFSET_MODE] >= 0.5f;
            const int  fadeN = declickSamples(v.fx[N_DECLICK]);
            const double stepAbs = std::fabs(v.step * v.modPitch);
            // Envelope segment hints live in LOCALS for the block: a per-sample
            // store through the member reference measurably taxed the loop
            // (~90 ns/output-sample across 32 voices); written back once below.
            int ampHint = v.ampEnvHint, modHint = v.modEnvHint;
            for (int i = 0; i < n; ++i) {
                // amp envelope: freeze at sustain until released
                float amp = 1.f;
                if (v.fadingOut) {
                    // A declick TAIL (a retired voice, or one cut by an
                    // exclusive class) is a frozen snapshot finishing its
                    // crossfade: its gain is the ramp's alone.  Stepping the
                    // envelope here would let a zone with a ZERO release -- i.e.
                    // most percussion -- cut its own tail dead on the spot, and
                    // the click the tail exists to prevent comes straight back.
                    amp = v.lastEnvAmp;
                    // A declick TAIL (retired voice, or one cut by an exclusive
                    // class) is a frozen snapshot finishing its crossfade: its
                    // gain is the ramp's alone.  Stepping its envelope would let
                    // a zone with a ZERO release cut the tail dead on the spot,
                    // reintroducing exactly the click the crossfade exists to
                    // remove.

                } else if (useAmp) {
                    amp = ampEnv.evalHinted(v.envPhase, ampHint);
                    v.lastEnvAmp = amp;
                    if (!(v.released) && v.envPhase < sustainX) {
                        v.envPhase += ampInc;
                        if (v.envPhase > sustainX) v.envPhase = sustainX;
                    } else if (v.released) {
                        v.envPhase += ampInc;
                    }
                }
                // mod envelope (0..1), same zone-else-instrument fallback
                float modLvl = 0.f;
                if (useModG) {
                    modLvl = modEnvG.evalHinted(v.envPhaseMod, modHint);
                    if (!(v.released) && v.envPhaseMod < modSustainX) {
                        v.envPhaseMod += envInc;
                        if (v.envPhaseMod > modSustainX) v.envPhaseMod = modSustainX;
                    } else if (v.released) {
                        v.envPhaseMod += envInc;
                    }
                }
                float sl = 0.f, sr = 0.f;
                if (granular) {
                    const int grainLife = std::max(8, (int)((0.002 + 0.498 * v.fx[N_GRAIN_SIZE]) * sr_));
                    const double density = 1.0 + 79.0 * v.fx[N_GRAIN_DENSITY];
                    if ((v.grainClock -= 1.0) <= 0.0) {
                        v.grainClock += sr_ / density;
                        for (int gi = 0; gi < Voice::kMaxGrains; ++gi) {
                            Voice::Grain& gr = v.grains[gi];
                            if (gr.active) continue;
                            auto rnd = [&]() -> float {
                                v.grainRng ^= v.grainRng << 13; v.grainRng ^= v.grainRng >> 17;
                                v.grainRng ^= v.grainRng << 5;
                                return (float)(v.grainRng & 0xffffffu) / (float)0xffffffu;
                            };
                            const double spray = (rnd() * 2.0 - 1.0) * v.fx[N_GRAIN_SPRAY] * z.numFrames * 0.25;
                            gr.active = true; gr.age = 0; gr.life = grainLife;
                            gr.pos = std::max(0.0, std::min((double)z.numFrames - 1.0, v.pos + spray));
                            const double jit = (rnd() * 2.0 - 1.0) * v.fx[N_GRAIN_JITTER] * 2.0;
                            gr.rate = v.step * v.modPitch * semisToRatio(jit);
                            gr.dir = (v.fx[N_GRAIN_REVERSE] > 0.f && rnd() < v.fx[N_GRAIN_REVERSE]) ? -1 : 1;
                            const float pan = (rnd() * 2.f - 1.f) * v.fx[N_GRAIN_SPREAD];
                            gr.panL = pan <= 0.f ? 1.f : 1.f - pan;
                            gr.panR = pan >= 0.f ? 1.f : 1.f + pan;
                            break;
                        }
                    }
                    int activeGrains = 0;
                    for (int gi = 0; gi < Voice::kMaxGrains; ++gi) {
                        Voice::Grain& gr = v.grains[gi];
                        if (!gr.active) continue;
                        if (gr.pos < 0.0 || gr.pos >= z.numFrames || gr.age >= gr.life) { gr.active = false; continue; }
                        float gl, grr; z.frame(gr.pos, gl, grr);
                        const float phase = (float)gr.age / (float)std::max(1, gr.life);
                        const float sine = std::sin(3.1415926535f * phase);
                        const float tri = 1.f - std::fabs(phase * 2.f - 1.f);
                        const float window = tri + (sine - tri) * v.fx[N_GRAIN_SHAPE];
                        sl += gl * window * gr.panL; sr += grr * window * gr.panR;
                        gr.pos += gr.rate * gr.dir; ++gr.age; ++activeGrains;
                    }
                    const float norm = 1.f / std::sqrt((float)std::max(1, activeGrains));
                    sl *= norm; sr *= norm;
                } else if (akai) {
                    renderSmear(v, z, sl, sr);
                } else {
                    z.frame(v.pos, sl, sr);
                }
                // PER-ZONE LOW-PASS (SF2 initialFilterFc / initialFilterQ), run
                // on this voice's own filter state so one note can never ring
                // another's filter.  Topology-preserving SVF: stable while the
                // mod envelope sweeps the cutoff, unlike a naive biquad.
                if (useFilter) {
                    float fc = v.cutoffHz;
                    if (v.modToFilterCents != 0.f)
                        fc *= std::pow(2.f, modLvl * v.modToFilterCents / 1200.f);
                    const float nyq = (float)sr_ * 0.49f;
                    if (fc < 20.f) fc = 20.f;
                    if (fc > nyq)  fc = nyq;
                    // Recompute the coefficients only when the cutoff actually
                    // moved: one compare per sample instead of a tan() per
                    // sample per voice for the (normal) unmodulated case.
                    if (v.svfCachedHz <= 0.f ||
                        std::fabs(fc - v.svfCachedHz) > v.svfCachedHz * 0.005f) {
                        const float gg = std::tan(3.14159265f * fc / (float)sr_);
                        const float kk = 1.f / v.filtQ;
                        v.svfA1 = 1.f / (1.f + gg * (gg + kk));
                        v.svfA2 = gg * v.svfA1;
                        v.svfA3 = gg * v.svfA2;
                        v.svfCachedHz = fc;
                    }
                    const float a1 = v.svfA1, a2 = v.svfA2, a3 = v.svfA3;
                    auto lp = [a1, a2, a3](float x, float& ic1, float& ic2) {
                        const float t3 = x - ic2;
                        const float t1 = a1 * ic1 + a2 * t3;
                        const float t2 = ic2 + a2 * ic1 + a3 * t3;
                        ic1 = 2.f * t1 - ic1; ic2 = 2.f * t2 - ic2;
                        return t2;
                    };
                    sl = lp(sl, v.svfIc1L, v.svfIc2L);
                    sr = lp(sr, v.svfIc1R, v.svfIc2R);
                }
                // per-column FX, on this voice's own audio and own state
                applyVoiceFx(v, sl, sr);
                // Declick / crossfade ramp, applied LAST so an outgoing tail and
                // the incoming voice sum to unity gain across the overlap.
                const float fade = declickShape(v.fadePhase);
                const float g = v.velGain * amp * ampScale * v.modGain * v.zoneGain * fade;
                // pan = the zone's position plus whatever the column asks for,
                // clamped (neutral at 0 -> identical to before)
                float pn = v.modPan + v.zonePan;
                if (pn < -1.f) pn = -1.f; else if (pn > 1.f) pn = 1.f;
                const float pL = pn <= 0.f ? 1.f : 1.f - pn;
                const float pR = pn >= 0.f ? 1.f : 1.f + pn;
                L[off + i] += sl * g * pL;
                if (R) R[off + i] += sr * g * pR;
                // mod envelope -> pitch (SF2 modEnvToPitch, in cents)
                double stepNow = v.step * v.modPitch;
                if (v.modToPitchCents != 0.f && useModG)
                    stepNow *= std::pow(2.0, (double)modLvl * (double)v.modToPitchCents / 1200.0);
                v.pos += stepNow;               // cloud centre in granular mode
                v.age += 1.0;
                if (v.fadeRate != 0.f) {
                    v.fadePhase += v.fadeRate;
                    if (v.fadePhase >= 1.f) { v.fadePhase = 1.f; v.fadeRate = 0.f; }
                    else if (v.fadePhase <= 0.f) {   // tail finished at exactly zero
                        v.fadePhase = 0.f; v.active = false; break;
                    }
                }
                // loop / end handling.  A zone with its own amp envelope keeps
                // LOOPING through its release -- otherwise a 2-second release on
                // a sustained loop would run off the end of the sample and die
                // early, which is the opposite of what the release asks for.
                if (z.loop && (!v.released || zoneAmp)) {
                    const double le = z.loopEnd > z.loopStart ? z.loopEnd : z.numFrames;
                    if (v.pos >= le) v.pos -= (le - z.loopStart);
                } else {
                    // Ramp OUT into the end of the sample rather than stopping
                    // dead on the last frame -- a one-shot whose tail is not at
                    // zero clicks just as loudly as a bad start.
                    const double left = (double)z.numFrames - 1.0 - v.pos;
                    // `v.fadeRate <= 0` excluded any voice still ramping IN, so a
                    // sample SHORTER than the declick window never armed its
                    // fade-out and was cut at full level on the last frame --
                    // the one case that clicks hardest.  A voice mid-fade-in gets
                    // its ramp reversed from wherever it currently is instead.
                    if (!v.fadingOut && fadeN > 0 && left <= (double)fadeN * stepAbs) {
                        v.fadingOut = true;
                        v.fadeRate = -1.f / (float)fadeN;
                    }
                    if (v.pos >= (double)z.numFrames - 1.0) { v.active = false; break; }
                }
                // amp envelope finished after release -> voice done
                if (useAmp && v.released && v.envPhase >= 1.0 && ampEnv.eval(1.0) <= 0.0001f) {
                    v.active = false; break;
                }
                // A zone envelope that has run past its release end is done --
                // the phase axis reaches 1.0 exactly at the end of the release
                // segment, so this is the same test the instrument path uses.
                if (useAmp && zoneAmp && v.released && v.envPhase >= 1.0 && !v.fadingOut) {
                    v.active = false; break;
                }
            }
            v.ampEnvHint = ampHint;
            v.modEnvHint = modHint;
        }
    }

    // Native FX tail: gain / pan / width / drive / LP / HP / bit / downsample / noise.
    void applyNativeFx(float* L, float* R, int n) {
        const float outGain = volumeNormToGain(native_[N_OUTPUT_GAIN]);
        const float pan = native_[N_PAN] * 2.f - 1.f;
        const float panL = pan <= 0.f ? 1.f : 1.f - pan;
        const float panR = pan >= 0.f ? 1.f : 1.f + pan;
        const float width = native_[N_WIDTH] * 2.f;
        const float drive = 1.f + native_[N_DRIVE] * 15.f;
        const bool mono = native_[N_MONO] > 0.5f;
        const bool swap = native_[N_SWAP] > 0.5f;
        const float lpHz = 80.f + native_[N_LP] * native_[N_LP] * 19920.f;
        const float hpHz = native_[N_HP] * native_[N_HP] * 4000.f;
        const float lpa = onepole_alpha(lpHz, sr_);
        const float hpa = onepole_alpha(std::max(10.f, hpHz), sr_);
        const int holdN = 1 + (int)std::lround(native_[N_DOWNSAMPLE] * 31.f);
        const float bitSteps = std::pow(2.f, 4.f + native_[N_BITDEPTH] * 12.f);
        const float noiseAmt = native_[N_NOISE] * 0.02f;
        const bool  driveOn  = native_[N_DRIVE] > 0.f;
        const float driveNorm = driveOn ? std::tanh(drive) : 1.f;
        for (int i = 0; i < n; ++i) {
            float l = L[i], r = R ? R[i] : L[i];
            if (swap) std::swap(l, r);
            float mid = (l + r) * 0.5f, side = (l - r) * 0.5f * width;
            l = mono ? mid : mid + side;
            r = mono ? mid : mid - side;
            lpStateL_ += lpa * (l - lpStateL_); lpStateR_ += lpa * (r - lpStateR_);
            l = lpStateL_; r = lpStateR_;
            if (hpHz > 1.f) {
                hpStateL_ += hpa * (l - hpStateL_); hpStateR_ += hpa * (r - hpStateR_);
                l -= hpStateL_; r -= hpStateR_;
            }
            // Decimator state must PERSIST across blocks.  `i % holdN` restarted
            // the phase at every block boundary and the held sample was a
            // block-local that began at 0, so the downsampler injected a step
            // (and a zero) every buffer -- audible as a buzz locked to the block
            // rate, not to the parameter.
            if (decPhase_ <= 0) { decHeldL_ = l; decHeldR_ = r; decPhase_ = holdN; }
            --decPhase_;
            l = decHeldL_; r = decHeldR_;
            l = std::round(l * bitSteps) / bitSteps; r = std::round(r * bitSteps) / bitSteps;
            rng_ = rng_ * 1664525u + 1013904223u;
            float nz = ((int)((rng_ >> 8) & 0xffff) / 32768.f - 1.f) * noiseAmt;
            l += nz; r += nz;
            // Drive at 0.0 is a bypass: tanh(x)/tanh(1) is ~1.31x, not unity.
            if (driveOn) {
                l = std::tanh(l * drive) / driveNorm;
                r = std::tanh(r * drive) / driveNorm;
            }
            L[i] = l * outGain * panL;
            if (R) R[i] = r * outGain * panR;
        }
    }

    static void sanitizeZone(SampleZone& z) {
        // Every read path dereferences `pcm`, so a zone never carries a null
        // one.  An empty shared buffer costs one tiny allocation and removes a
        // whole class of null check from the audio path.
        if (!z.pcm) { z.pcm = std::make_shared<const std::vector<float>>(); z.numFrames = 0; }
        z.slot = std::max(1, z.slot); z.level = std::max(0, z.level);
        z.rootKey = std::max(0, std::min(127, z.rootKey));
        z.loKey = std::max(0, std::min(127, z.loKey)); z.hiKey = std::max(0, std::min(127, z.hiKey));
        z.loVel = std::max(0, std::min(127, z.loVel)); z.hiVel = std::max(0, std::min(127, z.hiVel));
        z.overlapMode = std::max(0, std::min(2, z.overlapMode));
        if (z.hiKey < z.loKey) std::swap(z.loKey, z.hiKey);
        if (z.hiVel < z.loVel) std::swap(z.loVel, z.hiVel);
        if (z.sampleRate <= 0) z.sampleRate = 48000;
        // LOOP POINTS.  Every other field was clamped here and these two were
        // taken on trust -- from an API caller, or straight out of a project
        // blob in loadState().  A loopEnd past numFrames left the read pointer
        // walking off the end of the PCM, where the cubic reader clamps to the
        // last frame: hundreds of milliseconds of held DC every pass.  Worse,
        // loopStart > loopEnd made the wrap `pos -= (loopEnd - loopStart)`
        // ADVANCE the pointer, so the voice never reached its end, never
        // deactivated, and permanently occupied a polyphony slot.
        if (z.numFrames < 0) z.numFrames = 0;
        if (z.loopStart < 0) z.loopStart = 0;
        if (z.loopEnd <= 0 || z.loopEnd > z.numFrames) z.loopEnd = z.numFrames;
        // Points that still make no span (loopStart at or past loopEnd) are not
        // repaired into an invented loop -- the zone simply plays as the one-shot
        // it effectively is, and the voice ends instead of hanging.
        if (z.loopStart >= z.loopEnd) { z.loopStart = 0; z.loop = false; }
        sanitizeZoneParams(z);
    }

    //! Clamp the per-zone SF2-parity fields into ranges the render path can
    //! trust.  Called from sanitizeZone() (load / sample import) AND from every
    //! per-zone setter, so a hostile blob and a bad API call are covered by the
    //! same code -- a NaN cutoff or a negative decay would otherwise reach the
    //! audio thread.
    static void sanitizeZoneParams(SampleZone& z) {
        auto fix = [](float v, float lo, float hi, float dflt) {
            if (!(v == v)) return dflt;                 // NaN
            return v < lo ? lo : (v > hi ? hi : v);
        };
        auto fixEnv = [&](SamplerZoneEnv& e) {
            e.enabled = e.enabled ? 1 : 0;
            e.delay   = fix(e.delay,   0.f, kMaxEnvStageSec, 0.f);
            e.attack  = fix(e.attack,  0.f, kMaxEnvStageSec, 0.f);
            e.hold    = fix(e.hold,    0.f, 60.f, 0.f);
            e.decay   = fix(e.decay,   0.f, kMaxEnvStageSec, 0.f);
            e.release = fix(e.release, 0.f, kMaxEnvStageSec, 0.f);
            e.sustain = fix(e.sustain, 0.f, 1.f,  1.f);
        };
        fixEnv(z.ampEnv); fixEnv(z.modEnv);
        z.cutoffHz    = fix(z.cutoffHz, 0.f, 22050.f, 0.f);
        z.resonanceDb = fix(z.resonanceDb, 0.f, 48.f, 0.f);
        z.pan         = fix(z.pan, -1.f, 1.f, 0.f);
        z.attenuationDb = fix(z.attenuationDb, -24.f, 144.f, 0.f);
        z.modEnvToPitchCents  = fix(z.modEnvToPitchCents,  -12000.f, 12000.f, 0.f);
        z.modEnvToFilterCents = fix(z.modEnvToFilterCents, -12000.f, 12000.f, 0.f);
        z.coarseTune  = std::max(-120, std::min(120, z.coarseTune));
        z.fineTune    = std::max(-1200, std::min(1200, z.fineTune));
        z.scaleTuning = std::max(0, std::min(1200, z.scaleTuning));
        if (z.exclusiveClass < 0) z.exclusiveClass = 0;
    }

    PluginDescriptor        desc_;
    mutable std::mutex      mutex_;
    double                  sr_ = 48000.0;
    std::vector<SampleZone> zones_;
    // Zone lookup index (see the comment block above zoneForMidi): message-thread
    // maintained under mutex_, read-only for the audio thread.
    std::vector<int32_t>    keyZones_[128];
    std::unordered_map<uint64_t, int32_t> zoneBySlotLevel_;
    EnvelopeF               envs_[kNumEnvs];
    // [0, kMaxVoices) are note voices; [kMaxVoices, kTotalVoices) are the
    // reserved declick tails.  Every render/reset loop covers both.
    Voice                   voices_[kTotalVoices];
    // Seeded from nativeDefault() in the constructor so there is exactly ONE
    // table of defaults to keep in step when a parameter is appended.
    float                   native_[N_COUNT] = { 0.f };
    float                   nativeSeen_[N_COUNT] = { 0.f };   // last value pushed to voices
    float                   lpStateL_ = 0.f, lpStateR_ = 0.f, hpStateL_ = 0.f, hpStateR_ = 0.f;
    // Global decimator state -- a MEMBER, not a block local, so the sample-and-hold
    // clock is continuous across process() calls (see applyNativeFx).
    int                     decPhase_ = 0;
    float                   decHeldL_ = 0.f, decHeldR_ = 0.f;
    uint32_t                rng_ = 0x12345678u;
    // per-column routing: which tracker column each MIDI note belongs to, and the
    // per-column FX modulation the tracker's FX columns drive.
    int8_t                  noteColumn_[128];
    float                   colGain_[kMaxColumns];
    float                   colPan_[kMaxColumns];
    double                  colPitch_[kMaxColumns];
    // Per-column parameter values + a bitmask of which ids that column OWNS.
    // An unowned id falls through to native_, so a column only diverges from the
    // instrument for the FX its own tracker column actually drives.
    float                   colFx_[kMaxColumns][N_COUNT] = {};
    uint32_t                colSet_[kMaxColumns] = { 0u };
};

// --- factory + C-style API ---------------------------------------------------
IPluginInstance* create_sampler_instrument() { return new SamplerInstrument(); }

bool sampler_is_sampler(IPluginInstance* inst) {
    return dynamic_cast<SamplerInstrument*>(inst) != nullptr;
}

void sampler_set_note_column(IPluginInstance* inst, int note, int column) {
    if (auto* s = dynamic_cast<SamplerInstrument*>(inst)) s->setNoteColumn(note, column);
}
void sampler_set_column_param(IPluginInstance* inst, int column, int paramId, float value) {
    if (auto* s = dynamic_cast<SamplerInstrument*>(inst)) s->setColumnParam((uint32_t)paramId, value, column);
}

void sampler_load_sample(IPluginInstance* inst, int slot, int level,
                         const float* interleaved, int numFrames, bool stereo,
                         int rootMidiNote, int sampleRate, int loopStart, int loopEnd, bool loop,
                         int loKey, int hiKey, const char* name) {
    sampler_load_sample_ex(inst, slot, level, interleaved, numFrames, stereo, rootMidiNote,
                           sampleRate, loopStart, loopEnd, loop, loKey, hiKey, 0, 127,
                           false, true, true, 0, name);
}
void sampler_load_sample_ex(IPluginInstance* inst, int slot, int level,
                            const float* interleaved, int numFrames, bool stereo,
                            int rootMidiNote, int sampleRate, int loopStart, int loopEnd, bool loop,
                            int loKey, int hiKey, int loVel, int hiVel,
                            bool noteOffLayer, bool keyToPitch, bool velToVol,
                            int overlapMode, const char* name) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst))
        s->loadZone(slot, level, rootMidiNote, loKey, hiKey, loVel, hiVel, noteOffLayer,
                    keyToPitch, velToVol, overlapMode, interleaved, numFrames, stereo,
                    sampleRate, loopStart, loopEnd, loop, name);
}
void sampler_load_sample_shared(IPluginInstance* inst, int slot, int level,
                                SharedPcm pcm,
                                int numFrames, bool stereo, int rootMidiNote,
                                int sampleRate, int loopStart, int loopEnd, bool loop,
                                int loKey, int hiKey, int loVel, int hiVel,
                                bool noteOffLayer, bool keyToPitch, bool velToVol,
                                int overlapMode, const char* name) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst))
        s->loadZoneShared(slot, level, rootMidiNote, loKey, hiKey, loVel, hiVel, noteOffLayer,
                          keyToPitch, velToVol, overlapMode, std::move(pcm), numFrames, stereo,
                          sampleRate, loopStart, loopEnd, loop, name);
}
const float* sampler_zone_pcm_ptr(IPluginInstance* inst, int index) {
    SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst);
    return s ? s->zonePcmPtr(index) : nullptr;
}
void sampler_clear_slot(IPluginInstance* inst, int slot) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst)) s->clearSlot(slot);
}
void sampler_clear_all_zones(IPluginInstance* inst) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst)) s->clearAllZones();
}
void sampler_set_envelope(IPluginInstance* inst, int env, const unsigned short* xs,
                          const unsigned short* ys, const int* flags, int count) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst))
        if (count >= 0) s->setEnvelope(env, xs, ys, flags, count);
}
void sampler_set_zone_env(IPluginInstance* inst, int slot, int level, int env,
                          const SamplerZoneEnv* e) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst))
        s->setZoneEnv(slot, level, env, e);
}
bool sampler_get_zone_env(IPluginInstance* inst, int slot, int level, int env,
                          SamplerZoneEnv* out) {
    SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst);
    return s && s->getZoneEnv(slot, level, env, out);
}
void sampler_set_zone_filter(IPluginInstance* inst, int slot, int level,
                             float cutoffHz, float resonanceDb) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst))
        s->setZoneFilter(slot, level, cutoffHz, resonanceDb);
}
void sampler_set_zone_tuning(IPluginInstance* inst, int slot, int level,
                             int coarse, int fine, int scaleTuning) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst))
        s->setZoneTuning(slot, level, coarse, fine, scaleTuning);
}
void sampler_set_zone_level(IPluginInstance* inst, int slot, int level,
                            float pan, float attenuationDb) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst))
        s->setZoneLevel(slot, level, pan, attenuationDb);
}
void sampler_set_zone_exclusive(IPluginInstance* inst, int slot, int level,
                                int exclusiveClass) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst))
        s->setZoneExclusive(slot, level, exclusiveClass);
}
void sampler_set_zone_modroute(IPluginInstance* inst, int slot, int level,
                               float toPitchCents, float toFilterCents) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst))
        s->setZoneModRoute(slot, level, toPitchCents, toFilterCents);
}

int sampler_zone_count(IPluginInstance* inst) {
    SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst);
    return s ? s->exportZoneCount() : 0;
}
bool sampler_get_zone(IPluginInstance* inst, int index, SamplerZoneInfo& out) {
    SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst);
    return s && s->exportZone(index, out);
}
bool sampler_get_zone_meta(IPluginInstance* inst,int index,SamplerZoneInfo& out) {
    SamplerInstrument* s=dynamic_cast<SamplerInstrument*>(inst);
    return s&&s->exportZone(index,out,false);
}
bool sampler_has_envelopes(IPluginInstance* inst) {
    SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst);
    return s && s->hasEnvelopes();
}

}} // namespace PatchKnob::engine
