//----------------------------------------------------------------------------
//  src/engine/buzz/sampler_instrument.cpp
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
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace PatchKnob { namespace engine {

namespace {
constexpr int   kMaxVoices   = 32;
constexpr int   kMaxColumns  = 8;    // tracker note-columns; per-column voice + FX
constexpr int   kDefaultSlot = 1;
constexpr int   kNumEnvs     = 5;    // 0 amp, 1 pitch, 2 cutoff, 3 resonance, 4 pan
constexpr float kEnvSeconds  = 2.0f; // envelope x=0..1 spans this many seconds
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
} // namespace

// One point of a modulation envelope (editor's dense representation).
struct EnvPointF { float x = 0.f, y = 0.f; bool sustain = false; };
struct EnvelopeF {
    std::vector<EnvPointF> pts;
    int  sustainIdx = -1;
    bool enabled    = false;
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
    float sustainX() const {
        return (sustainIdx >= 0 && sustainIdx < (int)pts.size()) ? pts[(size_t)sustainIdx].x : 1.f;
    }
};

// A loaded multisample zone (owns its PCM so it is fully self-persisting).
struct SampleZone {
    int slot = kDefaultSlot, level = 0;
    int rootKey = 60, loKey = 0, hiKey = 127, loVel = 0, hiVel = 127;
    bool noteOffLayer = false, keyToPitch = true, velToVol = true;
    int overlapMode = 0;
    int sampleRate = 48000, loopStart = 0, loopEnd = 0;
    bool loop = false, stereo = false;
    int numFrames = 0;
    std::string name;
    std::vector<float> pcm;   // interleaved -1..1 (L,R,... if stereo)
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
    if (pcm.empty() || numFrames <= 0) { l = r = 0.f; return; }
    if (stereo) { l = cubicCh(pcm.data(),     numFrames, 2, pos);
                  r = cubicCh(pcm.data() + 1, numFrames, 2, pos); }
    else        { l = r = cubicCh(pcm.data(), numFrames, 1, pos); }
}

class SamplerInstrument : public IPluginInstance {
public:
    SamplerInstrument() {
        desc_.format = PluginFormat::VST2;
        desc_.name = "Sampler"; desc_.vendor = "PatchKnob";
        desc_.isInstrument = true; desc_.numAudioIn = 0; desc_.numAudioOut = 2;
        for (int i = 0; i < 128; ++i) noteColumn_[i] = -1;
        for (int c = 0; c < kMaxColumns; ++c) { colGain_[c] = 1.f; colPan_[c] = 0.f; colPitch_[c] = 1.0; }
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
        if (column < 0 || column >= kMaxColumns) { if (id < (uint32_t)N_COUNT) native_[id] = clamp01(value); return; }
        value = clamp01(value);
        if (id == (uint32_t)N_OUTPUT_GAIN) {
            colGain_[column] = value;
            for (Voice& v : voices_) if (v.active && v.column == column) v.modGain = value;
        } else if (id == (uint32_t)N_PAN) {
            const float p = value * 2.f - 1.f; colPan_[column] = p;
            for (Voice& v : voices_) if (v.active && v.column == column) v.modPan = p;
        } else if (id == (uint32_t)N_TRANSPOSE) {
            const double ratio = semisToRatio((value - 0.5) * 48.0); colPitch_[column] = ratio;
            for (Voice& v : voices_) if (v.active && v.column == column) v.modPitch = ratio;
        } else if (id < (uint32_t)N_COUNT) {
            native_[id] = value;   // not per-voice-safe yet -> global fallback
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
        std::lock_guard<std::mutex> lk(mutex_);
        std::vector<uint8_t> out;
        auto p32 = [&](uint32_t v){ for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(v >> (i * 8))); };
        auto pf  = [&](float f){ uint32_t v; std::memcpy(&v, &f, 4); p32(v); };
        auto ps  = [&](const std::string& s){ p32((uint32_t)s.size()); out.insert(out.end(), s.begin(), s.end()); };
        p32(0x53504d53u);                 // "SMPS"
        p32(4);                           // version 4: native-only sampler
        p32((uint32_t)N_COUNT); p32(0);   // nativeCount, buzzCount(=0, legacy slot)
        for (int i = 0; i < N_COUNT; ++i) pf(native_[i]);
        p32((uint32_t)zones_.size());
        for (const SampleZone& z : zones_) {
            p32((uint32_t)z.slot); p32((uint32_t)z.level);
            p32((uint32_t)z.rootKey); p32((uint32_t)z.loKey); p32((uint32_t)z.hiKey);
            p32((uint32_t)z.loVel); p32((uint32_t)z.hiVel);
            p32(z.noteOffLayer ? 1u : 0u); p32(z.keyToPitch ? 1u : 0u);
            p32(z.velToVol ? 1u : 0u); p32((uint32_t)z.overlapMode);
            p32((uint32_t)z.sampleRate); p32((uint32_t)z.loopStart); p32((uint32_t)z.loopEnd);
            p32(z.loop ? 1u : 0u); p32(z.stereo ? 1u : 0u); p32((uint32_t)z.numFrames);
            ps(z.name);
            p32((uint32_t)z.pcm.size());
            for (float s : z.pcm) pf(s);
        }
        p32((uint32_t)kNumEnvs);
        for (int e = 0; e < kNumEnvs; ++e) {
            p32((uint32_t)envs_[e].pts.size());
            for (const EnvPointF& pt : envs_[e].pts) {
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
        if (g32() != 0x53504d53u) return;
        uint32_t ver = g32();
        if (ver < 1 || ver > 4) return;
        uint32_t nativeCount = g32();
        uint32_t buzzCount   = g32();
        for (uint32_t i = 0; i < nativeCount && at + 4 <= d.size(); ++i) {
            float v = gf(); if (i < (uint32_t)N_COUNT) native_[i] = clamp01(v);
        }
        // legacy (v<=3) Buzz FX-param norms: skip -- this engine has no Buzz params.
        for (uint32_t i = 0; i < buzzCount && at + 4 <= d.size(); ++i) (void)gf();

        std::vector<SampleZone> zs;
        if (ver >= 2) {
            uint32_t zc = g32();
            if (zc > 512) return;
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
                uint32_t ns = g32();
                if (ns > 256u * 1024u * 1024u || at + (size_t)ns * 4u > d.size()) return;
                z.pcm.resize(ns);
                for (uint32_t i = 0; i < ns; ++i) z.pcm[i] = gf();
                sanitizeZone(z);
                const int ch = z.stereo ? 2 : 1;
                if (z.numFrames > 0 && (size_t)z.numFrames * (size_t)ch == z.pcm.size())
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
        if (haveEnvs) for (int e = 0; e < kNumEnvs; ++e) envs_[e] = std::move(es[e]);
    }

    // ---- sample / zone / envelope loading (called from the free functions) ---
    void loadZone(int slot, int level, int rootKey, int loKey, int hiKey, int loVel, int hiVel,
                  bool noteOffLayer, bool keyToPitch, bool velToVol, int overlapMode,
                  const float* interleaved, int numFrames, bool stereo, int sampleRate,
                  int loopStart, int loopEnd, bool loop, const char* name) {
        if (!interleaved || numFrames <= 0) return;
        std::lock_guard<std::mutex> lk(mutex_);
        SampleZone z;
        z.slot = slot < 1 ? kDefaultSlot : slot; z.level = std::max(0, level);
        z.rootKey = rootKey; z.loKey = loKey; z.hiKey = hiKey; z.loVel = loVel; z.hiVel = hiVel;
        z.noteOffLayer = noteOffLayer; z.keyToPitch = keyToPitch; z.velToVol = velToVol;
        z.overlapMode = overlapMode; z.stereo = stereo; z.numFrames = numFrames;
        z.sampleRate = sampleRate > 0 ? sampleRate : (int)sr_;
        z.loopStart = std::max(0, loopStart); z.loopEnd = loopEnd > 0 ? loopEnd : numFrames;
        z.loop = loop; z.name = name ? name : "";
        const int ch = stereo ? 2 : 1;
        z.pcm.assign(interleaved, interleaved + (size_t)numFrames * (size_t)ch);
        sanitizeZone(z);
        for (SampleZone& old : zones_)
            if (old.slot == z.slot && old.level == z.level) { killVoicesInSlot(z.slot); old = std::move(z); return; }
        zones_.push_back(std::move(z));
    }
    void clearSlot(int slot) {
        if (slot < 1) slot = kDefaultSlot;
        std::lock_guard<std::mutex> lk(mutex_);
        killVoicesInSlot(slot);
        zones_.erase(std::remove_if(zones_.begin(), zones_.end(),
                     [slot](const SampleZone& z){ return z.slot == slot; }), zones_.end());
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

    // ---- read-back for the editor to re-show state after a load -------------
    int  exportZoneCount() const { std::lock_guard<std::mutex> lk(mutex_); return (int)zones_.size(); }
    bool exportZone(int i, SamplerZoneInfo& z) const {
        std::lock_guard<std::mutex> lk(mutex_);
        if (i < 0 || i >= (int)zones_.size()) return false;
        const SampleZone& s = zones_[(size_t)i];
        z.slot = s.slot; z.level = s.level; z.rootKey = s.rootKey;
        z.loKey = s.loKey; z.hiKey = s.hiKey; z.loVel = s.loVel; z.hiVel = s.hiVel;
        z.noteOffLayer = s.noteOffLayer; z.keyToPitch = s.keyToPitch; z.velToVol = s.velToVol;
        z.overlapMode = s.overlapMode; z.sampleRate = s.sampleRate;
        z.loopStart = s.loopStart; z.loopEnd = s.loopEnd; z.loop = s.loop;
        z.stereo = s.stereo; z.numFrames = s.numFrames; z.name = s.name; z.pcm = s.pcm;
        return true;
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
        N_MONO, N_SWAP, N_NOISE, N_OFFSET, N_COUNT
    };
    static const char* nativeName(int i) {
        static const char* n[N_COUNT] = {
            "Output Gain","Pan","Stereo Width","Drive","Lowpass","Highpass",
            "Bit Depth","Downsample","Transpose","Velocity","Key Low","Key High",
            "Mono","Stereo Swap","Noise","Sample Offset" };
        return n[i];
    }
    static float nativeDefault(int i) {
        static const float d[N_COUNT] = { 0.5f,0.5f,0.5f,0.f,1.f,0.f,1.f,0.f,0.5f,1.f,0.f,1.f,0.f,0.f,0.f,0.f };
        return d[i];
    }

    // ---- one polyphonic voice ----------------------------------------------
    struct Voice {
        bool  active = false;
        int   note = -1;               // ORIGINAL MIDI note (for note-off match)
        int   column = -1;             // tracker note-column that triggered this voice
        const SampleZone* zone = nullptr;
        double pos = 0.0;              // fractional frame position
        double step = 1.0;            // frames advanced per output sample (pitch)
        float velGain = 1.0f;
        // per-COLUMN FX modulation (neutral by default -> untagged voices render
        // exactly as before; a column-tagged FX write updates ONLY this voice).
        float  modGain  = 1.0f;        // per-column volume (0..1 linear)
        float  modPan   = 0.0f;        // per-column pan (-1..1), applied pre-tail
        double modPitch = 1.0;         // per-column pitch ratio (multiplies step)
        // amp envelope
        bool  released = false;
        double envPhase = 0.0;        // 0..1 along the amp env
        float envLast = 0.f;
        double age = 0.0;             // for stealing (samples alive)
    };

    void handleMidi(const MidiEvent& m) {
        const unsigned char st = m.status & 0xF0u;
        if (st == 0x90u && m.data2 > 0) noteOn(m.data1, m.data2);
        else if (st == 0x80u || (st == 0x90u && m.data2 == 0)) noteOff(m.data1);
    }

    const SampleZone* zoneForMidi(int midiNote, int vel) const {
        const SampleZone* best = nullptr; int bestDist = 1 << 30;
        for (const SampleZone& z : zones_) {
            if (z.noteOffLayer) continue;
            if (midiNote < z.loKey || midiNote > z.hiKey) continue;
            if (vel < z.loVel || vel > z.hiVel) continue;
            int d = std::abs(midiNote - z.rootKey);
            if (d < bestDist) { bestDist = d; best = &z; }
        }
        return best;
    }

    int allocVoice(int column) {
        // Per-column mono: a new note in a TAGGED column reuses that column's own
        // voice (retrigger in-column) so a retrigger never spawns a stray voice and
        // columns never bleed into one another.  Untagged (column<0) keeps the
        // original per-note free/steal-oldest behavior.
        if (column >= 0) {
            int inCol = -1; double colAge = -1.0;
            for (int i = 0; i < kMaxVoices; ++i)
                if (voices_[i].active && voices_[i].column == column && voices_[i].age > colAge)
                    { colAge = voices_[i].age; inCol = i; }
            if (inCol >= 0) return inCol;
        }
        for (int i = 0; i < kMaxVoices; ++i) if (!voices_[i].active) return i;
        // steal the oldest voice
        int oldest = 0; double maxAge = -1.0;
        for (int i = 0; i < kMaxVoices; ++i) if (voices_[i].age > maxAge) { maxAge = voices_[i].age; oldest = i; }
        return oldest;
    }

    void noteOn(int midiNote, int vel) {
        const int low  = (int)std::lround(native_[N_KEY_LOW]  * 127.f);
        const int high = (int)std::lround(native_[N_KEY_HIGH] * 127.f);
        const int lo = std::min(low, high), hi = std::max(low, high);
        if (midiNote < lo || midiNote > hi) return;

        const int tr = (int)std::lround((native_[N_TRANSPOSE] - 0.5f) * 48.f);
        const int playNote = std::max(0, std::min(127, midiNote + tr));
        const SampleZone* zone = zoneForMidi(playNote, vel);
        if (!zone) return;

        int useVel = zone->velToVol ? vel : 127;
        useVel = std::max(1, std::min(127, (int)std::lround(useVel * native_[N_VELOCITY])));

        const int col = (midiNote >= 0 && midiNote < 128) ? (int)noteColumn_[midiNote] : -1;
        int vi = allocVoice(col);
        Voice& v = voices_[(size_t)vi];
        v = Voice{};
        v.active = true;
        v.note = midiNote;                 // key by original note for note-off
        v.column = col;
        if (col >= 0 && col < kMaxColumns) {   // seed the per-column FX modulation
            v.modGain = colGain_[col]; v.modPan = colPan_[col]; v.modPitch = colPitch_[col];
        }
        v.zone = zone;
        // Sample Offset (automatable, per-voice): start playback this far into the
        // sample.  Read at note-on so it's captured per-voice, not applied globally.
        v.pos = (double)clamp01(native_[N_OFFSET]) * (double)std::max(0, zone->numFrames - 1);
        const int triggerNote = zone->keyToPitch ? playNote : zone->rootKey;
        const double semis = triggerNote - zone->rootKey;
        v.step = semisToRatio(semis) * ((double)zone->sampleRate / sr_);
        v.velGain = (float)useVel / 127.f;
    }

    void noteOff(int midiNote) {
        for (Voice& v : voices_)
            if (v.active && v.note == midiNote && !v.released) {
                v.released = true;
                // jump the amp-env phase to (just past) the sustain point so the
                // release segment plays from here.
                if (envs_[0].enabled) {
                    double sx = envs_[0].sustainX();
                    if (v.envPhase < sx) v.envPhase = sx;
                }
            }
    }

    void killVoicesInSlot(int slot) {
        for (Voice& v : voices_) if (v.active && v.zone && v.zone->slot == slot) v = Voice{};
    }

    // Render all active voices additively into L/R for `n` samples from `off`.
    void renderVoices(float* L, float* R, int off, int n) {
        if (n <= 0) return;
        const float ampScale = 1.0f;
        const double envInc = 1.0 / (double)(kEnvSeconds * sr_);   // phase per sample
        for (Voice& v : voices_) {
            if (!v.active || !v.zone) continue;
            const SampleZone& z = *v.zone;
            const EnvelopeF& ampEnv = envs_[0];
            const bool useAmp = ampEnv.enabled;
            const double sustainX = ampEnv.sustainX();
            for (int i = 0; i < n; ++i) {
                // amp envelope: freeze at sustain until released
                float amp = 1.f;
                if (useAmp) {
                    amp = ampEnv.eval(v.envPhase);
                    if (!(v.released) && v.envPhase < sustainX) {
                        v.envPhase += envInc;
                        if (v.envPhase > sustainX) v.envPhase = sustainX;
                    } else if (v.released) {
                        v.envPhase += envInc;
                    }
                }
                float sl, sr; z.frame(v.pos, sl, sr);
                const float g = v.velGain * amp * ampScale * v.modGain;
                // per-column pan (neutral when modPan==0 -> identical to before)
                const float pL = v.modPan <= 0.f ? 1.f : 1.f - v.modPan;
                const float pR = v.modPan >= 0.f ? 1.f : 1.f + v.modPan;
                L[off + i] += sl * g * pL;
                if (R) R[off + i] += sr * g * pR;
                v.pos += v.step * v.modPitch;   // modPitch==1 -> unchanged
                v.age += 1.0;
                // loop / end handling
                if (z.loop && !v.released) {
                    const double le = z.loopEnd > z.loopStart ? z.loopEnd : z.numFrames;
                    if (v.pos >= le) v.pos -= (le - z.loopStart);
                } else {
                    if (v.pos >= (double)z.numFrames - 1.0) { v.active = false; break; }
                }
                // amp envelope finished after release -> voice done
                if (useAmp && v.released && v.envPhase >= 1.0 && ampEnv.eval(1.0) <= 0.0001f) {
                    v.active = false; break;
                }
            }
        }
    }

    // Native FX tail: gain / pan / width / drive / LP / HP / bit / downsample / noise.
    void applyNativeFx(float* L, float* R, int n) {
        const float outGain = db_to_gain(native_[N_OUTPUT_GAIN] * 48.f - 24.f);
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
        float heldL = 0.f, heldR = 0.f;
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
            if ((i % holdN) == 0) { heldL = l; heldR = r; }
            l = heldL; r = heldR;
            l = std::round(l * bitSteps) / bitSteps; r = std::round(r * bitSteps) / bitSteps;
            rng_ = rng_ * 1664525u + 1013904223u;
            float nz = ((int)((rng_ >> 8) & 0xffff) / 32768.f - 1.f) * noiseAmt;
            l = std::tanh((l + nz) * drive) / std::tanh(drive);
            r = std::tanh((r + nz) * drive) / std::tanh(drive);
            L[i] = l * outGain * panL;
            if (R) R[i] = r * outGain * panR;
        }
    }

    static void sanitizeZone(SampleZone& z) {
        z.slot = std::max(1, z.slot); z.level = std::max(0, z.level);
        z.rootKey = std::max(0, std::min(127, z.rootKey));
        z.loKey = std::max(0, std::min(127, z.loKey)); z.hiKey = std::max(0, std::min(127, z.hiKey));
        z.loVel = std::max(0, std::min(127, z.loVel)); z.hiVel = std::max(0, std::min(127, z.hiVel));
        z.overlapMode = std::max(0, std::min(2, z.overlapMode));
        if (z.hiKey < z.loKey) std::swap(z.loKey, z.hiKey);
        if (z.hiVel < z.loVel) std::swap(z.loVel, z.hiVel);
        if (z.sampleRate <= 0) z.sampleRate = 48000;
    }

    PluginDescriptor        desc_;
    mutable std::mutex      mutex_;
    double                  sr_ = 48000.0;
    std::vector<SampleZone> zones_;
    EnvelopeF               envs_[kNumEnvs];
    Voice                   voices_[kMaxVoices];
    float                   native_[N_COUNT] = { 0.5f,0.5f,0.5f,0.f,1.f,0.f,1.f,0.f,0.5f,1.f,0.f,1.f,0.f,0.f,0.f,0.f };
    float                   lpStateL_ = 0.f, lpStateR_ = 0.f, hpStateL_ = 0.f, hpStateR_ = 0.f;
    uint32_t                rng_ = 0x12345678u;
    // per-column routing: which tracker column each MIDI note belongs to, and the
    // per-column FX modulation the tracker's FX columns drive.
    int8_t                  noteColumn_[128];
    float                   colGain_[kMaxColumns];
    float                   colPan_[kMaxColumns];
    double                  colPitch_[kMaxColumns];
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
void sampler_clear_slot(IPluginInstance* inst, int slot) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst)) s->clearSlot(slot);
}
void sampler_set_envelope(IPluginInstance* inst, int env, const unsigned short* xs,
                          const unsigned short* ys, const int* flags, int count) {
    if (SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst))
        if (count >= 0) s->setEnvelope(env, xs, ys, flags, count);
}
int sampler_zone_count(IPluginInstance* inst) {
    SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst);
    return s ? s->exportZoneCount() : 0;
}
bool sampler_get_zone(IPluginInstance* inst, int index, SamplerZoneInfo& out) {
    SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst);
    return s && s->exportZone(index, out);
}
bool sampler_has_envelopes(IPluginInstance* inst) {
    SamplerInstrument* s = dynamic_cast<SamplerInstrument*>(inst);
    return s && s->hasEnvelopes();
}

}} // namespace PatchKnob::engine
