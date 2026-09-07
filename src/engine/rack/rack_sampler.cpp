//----------------------------------------------------------------------------
//  rack_sampler.cpp -- SMPL-1: a one-shot / looping sample player.
//
//  Deliberately ONE sample per module, with NO keyranges: the multisample,
//  key-mapped instrument is the Buzz-hosted "Sampler" node.  This is the
//  modular counterpart -- patch one per voice, one per drum, one per layer --
//  so keyranges would only get in the way.
//
//  Every automatable parameter has BOTH a knob and its own CV jack; the CV is
//  summed with the knob (+/-5 V == full range unless noted), which is the
//  normal modular convention and means any parameter can be swept from ENV-8,
//  an LFO, or a sequencer without a separate modulation matrix.
//
//  Sample data is owned by the module and swapped under a mutex the audio
//  thread only ever TRY-locks (the CsoundNode pattern), so a load can never
//  block or tear the render.  It implements rackx::ISampleSlot, which is what
//  puts "Load Sample..." on its right-click menu.
//----------------------------------------------------------------------------
#include "rack_factory.h"
#include "rack_panel_kit.h"
#include "../audioclip/audio_clip.h"
#include "../audioclip/wav_loader.h"
#include "../sample_ops.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace {

using rack::engine::Module;

constexpr float kGateHigh = 2.f;
constexpr float kGateLow  = 0.1f;

enum LoopMode { LOOP_OFF = 0, LOOP_FWD, LOOP_PINGPONG, LOOP_MODES };
enum TrigMode { TRIG_GATE = 0, TRIG_ONESHOT, TRIG_MODES };

//! Sum a knob with its CV jack.  +/-5 V spans the full parameter range.
//! knob + (outer-ring depth) * jack.  A disconnected jack contributes nothing
//! whatever the ring says, so a parked ring is always safe.
inline float cvSum(float knob, float depth, const rack::engine::Input& in,
                   float lo, float hi) {
    float v = knob;
    if (in.isConnected()) v += depth * in.getVoltage() * 0.1f * (hi - lo);
    return rack::clamp(v, lo, hi);
}

struct Sampler1 final : Module, rackx::ISampleSlot {
    enum ParamIds {
        TUNE_PARAM, FINE_PARAM,
        START_PARAM, END_PARAM,
        LOOPMODE_PARAM, LOOPSTART_PARAM, LOOPEND_PARAM, LOOPXFADE_PARAM,
        LEVEL_PARAM, PAN_PARAM,
        ATTACK_PARAM, DECAY_PARAM, SUSTAIN_PARAM, RELEASE_PARAM,
        CUTOFF_PARAM, RESO_PARAM,
        REVERSE_PARAM, TRIGMODE_PARAM,
        // granular
        GRAIN_PARAM,        // 0 = classic playback, 1 = granular
        GSIZE_PARAM, GDENS_PARAM, GSPRAY_PARAM, GJITTER_PARAM,
        GSHAPE_PARAM, GSPREAD_PARAM, GREVP_PARAM,
        // carries no value; the inline waveform canvas needs an id of its
        // own because find_element() keys panel elements on id
        WAVE_UI_PARAM,
        // House control unit (rack_panel_kit.h): a CV-depth ring and an inert
        // readout key per param, both APPENDED so saved patches keep indices.
        CVDEPTH_BASE,
        READOUT_BASE = CVDEPTH_BASE + CVDEPTH_BASE,
        NUM_PARAMS   = READOUT_BASE + CVDEPTH_BASE
    };
    // Every continuous parameter above gets a CV jack, in the same order.
    enum InputIds {
        PITCH_INPUT, GATE_INPUT,
        TUNE_CV, FINE_CV, START_CV, END_CV, LOOPSTART_CV, LOOPEND_CV,
        LEVEL_CV, PAN_CV, ATTACK_CV, DECAY_CV, SUSTAIN_CV, RELEASE_CV,
        CUTOFF_CV, RESO_CV,
        GSIZE_CV, GDENS_CV, GSPRAY_CV, GJITTER_CV,
        NUM_INPUTS
    };
    enum OutputIds { LEFT_OUTPUT, RIGHT_OUTPUT, EOC_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { PLAY_LIGHT, LOAD_LIGHT, NUM_LIGHTS };

    // ---- sample storage (message thread writes under mutex_) ---------------
    std::mutex          mutex_;
    std::vector<float>  chL_, chR_;
    double              clipRate_ = 48000.0;
    std::string         name_;
    std::string         path_;      // full source path, for project save/load

    // ---- voice state (audio thread) ---------------------------------------
    double pos_ = 0.0;
    int    dir_ = 1;
    bool   playing_ = false, gateHigh_ = false;
    float  env_ = 0.f;
    int    stage_ = 0;              // 0 idle, 1 A, 2 D, 3 S, 4 R
    float  lpL_ = 0.f, bpL_ = 0.f;  // SVF state, LEFT
    float  lpR_ = 0.f, bpR_ = 0.f;  // SVF state, RIGHT
    float  eoc_ = 0.f;
    // Last emitted output, plus the playback geometry that produced it.  Used
    // to ride out a block in which the GUI thread owns mutex_ (see process).
    float  lastL_ = 0.f, lastR_ = 0.f;
    double heldRate_ = 0.0;         // signed frames/sample of the last good block
    double heldFs_ = 0.0, heldFe_ = 0.0, heldFls_ = 0.0, heldFle_ = 0.0;
    int    heldLoopMode_ = LOOP_OFF;

    // ---- edit history (message thread) -------------------------------------
    struct Snap { std::vector<float> l, r; };
    std::vector<Snap> undo_, redo_;
    std::vector<float> clipL_, clipR_;      // cut/copy clipboard

    // ---- granular voices ---------------------------------------------------
    // A grain is a short windowed read of the sample at its own rate.  Grains
    // are launched on a schedule and die on their own, so density and size are
    // independent -- overlapping grains are what makes the texture.
    struct Grain {
        bool   on = false;
        double pos = 0.0;      // read position in frames
        double rate = 1.0;
        int    age = 0, life = 0;
        float  panL = 1.f, panR = 1.f;
        int    dir = 1;
    };
    static const int kMaxGrains = 24;
    Grain grains_[kMaxGrains];
    double grainClock_ = 0.0;      // frames until the next launch
    unsigned rngState_ = 0x9E3779B9u;

    //! Cheap xorshift; a grain cloud needs a lot of randoms per second and
    //! std::rand() is both slower and not safe to lean on from the audio thread.
    inline float frand() {
        rngState_ ^= rngState_ << 13; rngState_ ^= rngState_ >> 17;
        rngState_ ^= rngState_ << 5;
        return (float)( rngState_ & 0xFFFFFF ) / (float)0xFFFFFF;
    }

    //! An edit changes the buffer under the playing voice; park it rather than
    //! let pos_ point into audio that just moved.
    void stopVoice() {
        playing_ = false; stage_ = 0; env_ = 0.f; pos_ = 0.0; dir_ = 1;
    }

    Sampler1() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(TUNE_PARAM, -24.f, 24.f, 0.f, "Tune", " semi");
        configParam(FINE_PARAM, -1.f, 1.f, 0.f, "Fine", " semi");
        configParam(START_PARAM, 0.f, 1.f, 0.f, "Start");
        configParam(END_PARAM, 0.f, 1.f, 1.f, "End");
        configParam(LOOPMODE_PARAM, 0.f, (float)(LOOP_MODES - 1), 0.f, "Loop mode");
        configParam(LOOPSTART_PARAM, 0.f, 1.f, 0.f, "Loop start");
        configParam(LOOPEND_PARAM, 0.f, 1.f, 1.f, "Loop end");
        configParam(LOOPXFADE_PARAM, 0.f, 0.5f, 0.f, "Loop crossfade");
        configParam(LEVEL_PARAM, 0.f, 1.f, 0.8f, "Level");
        configParam(PAN_PARAM, -1.f, 1.f, 0.f, "Pan");
        configParam(ATTACK_PARAM, 0.f, 1.f, 0.01f, "Attack");
        configParam(DECAY_PARAM, 0.f, 1.f, 0.2f, "Decay");
        configParam(SUSTAIN_PARAM, 0.f, 1.f, 1.f, "Sustain");
        configParam(RELEASE_PARAM, 0.f, 1.f, 0.15f, "Release");
        configParam(CUTOFF_PARAM, 0.f, 1.f, 1.f, "Cutoff");
        configParam(RESO_PARAM, 0.f, 1.f, 0.f, "Resonance");
        configParam(REVERSE_PARAM, 0.f, 1.f, 0.f, "Reverse");
        configParam(TRIGMODE_PARAM, 0.f, (float)(TRIG_MODES - 1), 0.f, "Trigger mode");
        configParam(WAVE_UI_PARAM, 0.f, 1.f, 0.f, "Waveform");
        // Rings default CENTRED: a patched jack does nothing until dialled in.
        for (int p = 0; p < CVDEPTH_BASE; ++p)
            configParam(CVDEPTH_BASE + p, -1.f, 1.f, 0.f, "CV depth");
        configParam(GRAIN_PARAM, 0.f, 1.f, 0.f, "Granular");
        configParam(GSIZE_PARAM, 0.f, 1.f, 0.3f, "Grain size");
        configParam(GDENS_PARAM, 0.f, 1.f, 0.5f, "Grain density");
        configParam(GSPRAY_PARAM, 0.f, 1.f, 0.f, "Spray");
        configParam(GJITTER_PARAM, 0.f, 1.f, 0.f, "Pitch jitter");
        configParam(GSHAPE_PARAM, 0.f, 1.f, 0.5f, "Grain shape");
        configParam(GSPREAD_PARAM, 0.f, 1.f, 0.f, "Stereo spread");
        configParam(GREVP_PARAM, 0.f, 1.f, 0.f, "Reverse grains");
        configInput(GSIZE_CV, "Size CV");     configInput(GDENS_CV, "Density CV");
        configInput(GSPRAY_CV, "Spray CV");   configInput(GJITTER_CV, "Jitter CV");
        configInput(PITCH_INPUT, "V/Oct");
        configInput(GATE_INPUT, "Gate");
        configInput(TUNE_CV, "Tune CV");        configInput(FINE_CV, "Fine CV");
        configInput(START_CV, "Start CV");      configInput(END_CV, "End CV");
        configInput(LOOPSTART_CV, "Loop start CV");
        configInput(LOOPEND_CV, "Loop end CV");
        configInput(LEVEL_CV, "Level CV");      configInput(PAN_CV, "Pan CV");
        configInput(ATTACK_CV, "Attack CV");    configInput(DECAY_CV, "Decay CV");
        configInput(SUSTAIN_CV, "Sustain CV");  configInput(RELEASE_CV, "Release CV");
        configInput(CUTOFF_CV, "Cutoff CV");    configInput(RESO_CV, "Reso CV");
        configOutput(LEFT_OUTPUT, "L");
        configOutput(RIGHT_OUTPUT, "R");
        configOutput(EOC_OUTPUT, "EOC");
        configLight(PLAY_LIGHT, "Play");
        configLight(LOAD_LIGHT, "Loaded");
    }

    // ---- rackx::ISampleSlot (message thread) -------------------------------
    bool sampleLoad(const std::string& path, double sampleRate,
                    std::string* error) override {
        PatchKnob::engine::AudioClip clip;
        if (!PatchKnob::engine::loadWav(path, sampleRate, clip, error)) return false;
        if (clip.empty()) { if (error) *error = "sample is empty"; return false; }
        // Decode fully OUTSIDE the lock, then swap: the audio thread only ever
        // try-locks, so the render never waits on file I/O.
        std::lock_guard<std::mutex> lk(mutex_);
        // Decide mono-vs-stereo BEFORE moving anything: swapping ch[0] first
        // left the PREVIOUS sample's left channel sitting in clip.ch[0], so a
        // mono file then took the old audio as its right channel.
        const bool mono = clip.ch[1].empty() || clip.ch[1].size() != clip.ch[0].size();
        chL_ = std::move(clip.ch[0]);
        chR_ = mono ? chL_ : std::move(clip.ch[1]);
        undo_.clear(); redo_.clear();          // history belongs to the old file
        clipRate_ = clip.sampleRate > 0.0 ? clip.sampleRate : sampleRate;
        const size_t slash = path.find_last_of("/\\");
        name_ = (slash == std::string::npos) ? path : path.substr(slash + 1);
        path_ = path;                     // full path: what the project saves
        pos_ = 0.0; playing_ = false; env_ = 0.f; stage_ = 0;
        return true;
    }
    void sampleClear() override {
        std::lock_guard<std::mutex> lk(mutex_);
        chL_.clear(); chR_.clear(); name_.clear(); path_.clear();
        pos_ = 0.0; playing_ = false; env_ = 0.f; stage_ = 0;
    }
    const char* sampleName() const override { return name_.c_str(); }
    const char* samplePath() const override { return path_.c_str(); }

    //! Copy the audio out for the project file.  Const, but it still has to take
    //! the lock: the audio thread swaps these buffers under it.
    bool sampleReadAudio(std::vector<float>& outL, std::vector<float>& outR,
                         double& outRate) const override {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mutex_));
        if (chL_.empty()) return false;
        outL = chL_;
        outR = chR_.size() == chL_.size() ? chR_ : chL_;
        outRate = clipRate_ > 0.0 ? clipRate_ : 48000.0;
        return true;
    }

    //! Install audio straight from the project, skipping the WAV decoder, so an
    //! edited/reversed/trimmed sample comes back exactly as it was saved rather
    //! than being re-read from a source file that no longer matches.
    bool sampleSetAudio(const std::vector<float>& inL, const std::vector<float>& inR,
                        double rate, const std::string& name,
                        const std::string& path) override {
        if (inL.empty()) return false;
        std::lock_guard<std::mutex> lk(mutex_);
        chL_ = inL;
        chR_ = (inR.size() == inL.size()) ? inR : inL;
        undo_.clear(); redo_.clear();
        clipRate_ = rate > 0.0 ? rate : 48000.0;
        name_ = name;
        path_ = path;
        stopVoice();
        return true;
    }
    int sampleFrames() const override { return (int)chL_.size(); }
    double sampleRate() const override { return clipRate_; }

    //! Min/max peaks over [from,to).  Takes the module's own lock, so the UI
    //! never holds a pointer into audio a concurrent load may swap.
    int samplePeaks( int64_t from, int64_t to,
                     float* outMin, float* outMax, int buckets ) const override {
        if ( !outMin || !outMax || buckets <= 0 ) return 0;
        std::lock_guard<std::mutex> lk( const_cast<std::mutex&>( mutex_ ) );
        const int64_t n = (int64_t)chL_.size();
        if ( n <= 0 ) return 0;
        if ( from < 0 ) from = 0;
        if ( to > n ) to = n;
        if ( to <= from ) return 0;
        const double per = (double)( to - from ) / (double)buckets;
        for ( int b = 0; b < buckets; ++b ) {
            int64_t s = from + (int64_t)( per * b );
            int64_t e = from + (int64_t)( per * ( b + 1 ) );
            if ( e <= s ) e = s + 1;
            if ( e > n ) e = n;
            float mn = 1.f, mx = -1.f;
            for ( int64_t i = s; i < e; ++i ) {
                // Mono-sum for display; the editor draws one bipolar lane.
                const float v = 0.5f * ( chL_[(size_t)i] + chR_[(size_t)i] );
                if ( v < mn ) mn = v;
                if ( v > mx ) mx = v;
            }
            if ( mx < mn ) { mn = mx = 0.f; }
            outMin[b] = mn; outMax[b] = mx;
        }
        return buckets;
    }

    float sampleMarker( int m ) const override {
        switch ( m ) {
            case PatchKnob::engine::SM_START:      return params[START_PARAM].getValue();
            case PatchKnob::engine::SM_END:        return params[END_PARAM].getValue();
            case PatchKnob::engine::SM_LOOP_START: return params[LOOPSTART_PARAM].getValue();
            case PatchKnob::engine::SM_LOOP_END:   return params[LOOPEND_PARAM].getValue();
            default: return 0.f;
        }
    }
    void sampleSetMarker( int m, float v ) override {
        v = rack::clamp( v, 0.f, 1.f );
        switch ( m ) {
            case PatchKnob::engine::SM_START:      params[START_PARAM].setValue( v ); break;
            case PatchKnob::engine::SM_END:        params[END_PARAM].setValue( v ); break;
            case PatchKnob::engine::SM_LOOP_START: params[LOOPSTART_PARAM].setValue( v ); break;
            case PatchKnob::engine::SM_LOOP_END:   params[LOOPEND_PARAM].setValue( v ); break;
            default: break;
        }
    }
    bool sampleLoopEnabled() const override {
        return (int)std::lround( params[LOOPMODE_PARAM].getValue() ) != LOOP_OFF;
    }
    void sampleSetLoopEnabled( bool on ) override {
        params[LOOPMODE_PARAM].setValue( on ? (float)LOOP_FWD : (float)LOOP_OFF );
    }

    bool  sampleXfadeSupported() const override { return true; }
    float sampleXfade() const override { return params[LOOPXFADE_PARAM].getValue(); }
    void  sampleSetXfade( float v ) override {
        params[LOOPXFADE_PARAM].setValue( rack::clamp( v, 0.f, 0.5f ) );
    }

    //! Loop markers only mean something while looping is on.
    bool sampleMarkerActive( int m ) const override {
        if ( m == PatchKnob::engine::SM_LOOP_START || m == PatchKnob::engine::SM_LOOP_END )
            return (int)std::lround( params[LOOPMODE_PARAM].getValue() ) != LOOP_OFF;
        return true;
    }

    // ---- destructive editing (message thread, under mutex_) -----------------
    bool sampleEditable() const override { return !chL_.empty(); }
    bool sampleCanUndo() const override { return !undo_.empty(); }
    bool sampleCanRedo() const override { return !redo_.empty(); }

    //! Snapshot before every op.  Bounded, because a long sample is megabytes
    //! and an unbounded history would quietly eat the heap.
    void pushUndo() {
        undo_.push_back( Snap{ chL_, chR_ } );
        if ( undo_.size() > 16 ) undo_.erase( undo_.begin() );
        redo_.clear();
    }
    bool sampleUndo() override {
        std::lock_guard<std::mutex> lk( mutex_ );
        if ( undo_.empty() ) return false;
        redo_.push_back( Snap{ chL_, chR_ } );
        chL_ = undo_.back().l; chR_ = undo_.back().r;
        undo_.pop_back();
        stopVoice();
        return true;
    }
    bool sampleRedo() override {
        std::lock_guard<std::mutex> lk( mutex_ );
        if ( redo_.empty() ) return false;
        undo_.push_back( Snap{ chL_, chR_ } );
        chL_ = redo_.back().l; chR_ = redo_.back().r;
        redo_.pop_back();
        stopVoice();
        return true;
    }

    int64_t sampleZeroCross( int64_t frame, int dir ) const override {
        std::lock_guard<std::mutex> lk( const_cast<std::mutex&>( mutex_ ) );
        return PatchKnob::engine::find_zero_cross( chL_, frame, dir );
    }

    bool sampleOp( int op, int64_t from, int64_t to, float arg ) override {
        std::lock_guard<std::mutex> lk( mutex_ );
        if ( chL_.empty() ) return false;
        pushUndo();
        // The edits themselves live in engine/sample_ops.h so this module and
        // the Buzz sampler adapter run the SAME code.
        const bool changed = PatchKnob::engine::apply_sample_op(
            op, chL_, chR_, clipRate_, from, to, arg, clipL_, clipR_ );
        if ( !changed ) { undo_.pop_back(); return op == PatchKnob::engine::SOP_COPY; }
        stopVoice();
        return true;
    }

    bool sampleHasClipboard() const override { return !clipL_.empty(); }

    bool sampleStats( int64_t from, int64_t to, float* peak, float* rms ) const override {
        std::lock_guard<std::mutex> lk( const_cast<std::mutex&>( mutex_ ) );
        const int64_t n = (int64_t)chL_.size();
        if ( n <= 0 ) return false;
        from = std::max<int64_t>( 0, std::min( from, n ) );
        to   = std::max<int64_t>( 0, std::min( to, n ) );
        if ( to <= from ) return false;
        float pk = 0.f; double acc = 0.0;
        for ( int64_t i = from; i < to; ++i ) {
            const float m = 0.5f * ( chL_[(size_t)i] + chR_[(size_t)i] );
            pk = std::max( pk, std::fabs( m ) );
            acc += (double)m * (double)m;
        }
        if ( peak ) *peak = pk;
        if ( rms )  *rms  = (float)std::sqrt( acc / (double)( to - from ) );
        return true;
    }

    bool sampleSave( const std::string& path, std::string* error ) override {
        PatchKnob::engine::AudioClip clip;
        {
            std::lock_guard<std::mutex> lk( mutex_ );
            if ( chL_.empty() ) { if ( error ) *error = "nothing to save"; return false; }
            clip.ch[0] = chL_; clip.ch[1] = chR_;
            clip.sampleRate = clipRate_; clip.sourceSampleRate = clipRate_;
            clip.name = name_;
        }
        return PatchKnob::engine::saveWav16( path, clip, error );
    }

    void onReset() override {
        pos_ = 0.0; dir_ = 1; playing_ = false; gateHigh_ = false;
        env_ = 0.f; stage_ = 0; lpL_ = bpL_ = lpR_ = bpR_ = 0.f; eoc_ = 0.f;
        lastL_ = lastR_ = 0.f; heldRate_ = 0.0;
    }

    //! Linear-interpolated read; `p` is a frame position.
    inline void readAt(double p, float& l, float& r) const {
        const int64_t n = (int64_t)chL_.size();
        if (n <= 0) { l = r = 0.f; return; }
        if (p < 0.0) p = 0.0;
        if (p >= (double)(n - 1)) { l = chL_[(size_t)(n - 1)]; r = chR_[(size_t)(n - 1)]; return; }
        const int64_t i = (int64_t)p;
        const float f = (float)(p - (double)i);
        const size_t a = (size_t)i, b = (size_t)(i + 1);
        l = chL_[a] + (chL_[b] - chL_[a]) * f;
        r = chR_[a] + (chR_[b] - chR_[a]) * f;
    }

    void process(const ProcessArgs& args) override {
        // A load holds the mutex; skip this block rather than block the render.
        std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
        const bool have = lk.owns_lock() && !chL_.empty();
        lights[LOAD_LIGHT].setBrightnessRGB(have ? 0.1f : 0.f, have ? 0.7f : 0.f,
                                            have ? 0.2f : 0.f);
        if (!have) {
            if (!lk.owns_lock()) {
                // The GUI thread owns the mutex -- a load, a waveform repaint,
                // or samplePeaks() walking the visible span (the slot editor
                // re-extracts peaks every frame while you zoom).  Writing 0
                // here punched a hard digital click into the output on every
                // one of those frames, and returning without advancing pos_
                // stalled playback timing on top of the dropout.
                //
                // Instead: hold the last emitted sample and keep the playhead
                // moving on the geometry the last good block used.  The hold
                // decays gently, so a pathological multi-hundred-millisecond
                // lock fades out rather than latching a DC offset; over a
                // realistic sub-millisecond hold it is indistinguishable from
                // the sample itself.  Nothing here touches chL_/chR_, so it is
                // safe without the lock.
                constexpr float kHoldDecay = 0.9995f;   // ~2 s to -60 dB
                lastL_ *= kHoldDecay; lastR_ *= kHoldDecay;
                if (playing_ && heldRate_ != 0.0) {
                    pos_ += heldRate_;
                    if (heldLoopMode_ == LOOP_PINGPONG) {
                        if (pos_ >= heldFle_)      { pos_ = heldFle_; heldRate_ = -heldRate_; dir_ = -1; }
                        else if (pos_ <= heldFls_) { pos_ = heldFls_; heldRate_ = -heldRate_; dir_ =  1; }
                    } else if (heldLoopMode_ == LOOP_FWD) {
                        if (heldRate_ > 0.0 && pos_ >= heldFle_)      pos_ = heldFls_ + (pos_ - heldFle_);
                        else if (heldRate_ < 0.0 && pos_ <= heldFls_) pos_ = heldFle_ - (heldFls_ - pos_);
                    } else if (pos_ < heldFs_) pos_ = heldFs_;
                    else if (pos_ > heldFe_)   pos_ = heldFe_;
                }
                outputs[LEFT_OUTPUT].setVoltage(lastL_);
                outputs[RIGHT_OUTPUT].setVoltage(lastR_);
                if (eoc_ > 0.f) eoc_ -= args.sampleTime;
                outputs[EOC_OUTPUT].setVoltage(eoc_ > 0.f ? 10.f : 0.f);
            } else {
                // Lock held, but there is genuinely no sample: silence is right.
                lastL_ = lastR_ = 0.f;
                outputs[LEFT_OUTPUT].setVoltage(0.f);
                outputs[RIGHT_OUTPUT].setVoltage(0.f);
                outputs[EOC_OUTPUT].setVoltage(0.f);
            }
            outputs[LEFT_OUTPUT].channels = outputs[RIGHT_OUTPUT].channels = 1;
            return;
        }
        const int64_t n = (int64_t)chL_.size();

        // ---- parameters (knob + CV) ---------------------------------------
        const float tune = cvSum(params[TUNE_PARAM].getValue(), params[CVDEPTH_BASE + TUNE_PARAM].getValue(), inputs[TUNE_CV], -24.f, 24.f);
        const float fine = cvSum(params[FINE_PARAM].getValue(), params[CVDEPTH_BASE + FINE_PARAM].getValue(), inputs[FINE_CV], -1.f, 1.f);
        float sStart = cvSum(params[START_PARAM].getValue(), params[CVDEPTH_BASE + START_PARAM].getValue(), inputs[START_CV], 0.f, 1.f);
        float sEnd   = cvSum(params[END_PARAM].getValue(), params[CVDEPTH_BASE + END_PARAM].getValue(),   inputs[END_CV],   0.f, 1.f);
        if (sEnd <= sStart) sEnd = std::min(1.f, sStart + 1e-4f);
        float lStart = cvSum(params[LOOPSTART_PARAM].getValue(), params[CVDEPTH_BASE + LOOPSTART_PARAM].getValue(), inputs[LOOPSTART_CV], 0.f, 1.f);
        float lEnd   = cvSum(params[LOOPEND_PARAM].getValue(), params[CVDEPTH_BASE + LOOPEND_PARAM].getValue(),   inputs[LOOPEND_CV],   0.f, 1.f);
        if (lEnd <= lStart) lEnd = std::min(1.f, lStart + 1e-4f);
        const float level = cvSum(params[LEVEL_PARAM].getValue(), params[CVDEPTH_BASE + LEVEL_PARAM].getValue(), inputs[LEVEL_CV], 0.f, 1.f);
        const float pan   = cvSum(params[PAN_PARAM].getValue(), params[CVDEPTH_BASE + PAN_PARAM].getValue(),   inputs[PAN_CV],  -1.f, 1.f);
        const float aT = cvSum(params[ATTACK_PARAM].getValue(), params[CVDEPTH_BASE + ATTACK_PARAM].getValue(),  inputs[ATTACK_CV],  0.f, 1.f);
        const float dT = cvSum(params[DECAY_PARAM].getValue(), params[CVDEPTH_BASE + DECAY_PARAM].getValue(),   inputs[DECAY_CV],   0.f, 1.f);
        const float sL = cvSum(params[SUSTAIN_PARAM].getValue(), params[CVDEPTH_BASE + SUSTAIN_PARAM].getValue(), inputs[SUSTAIN_CV], 0.f, 1.f);
        const float rT = cvSum(params[RELEASE_PARAM].getValue(), params[CVDEPTH_BASE + RELEASE_PARAM].getValue(), inputs[RELEASE_CV], 0.f, 1.f);
        const float cut = cvSum(params[CUTOFF_PARAM].getValue(), params[CVDEPTH_BASE + CUTOFF_PARAM].getValue(), inputs[CUTOFF_CV], 0.f, 1.f);
        const float res = cvSum(params[RESO_PARAM].getValue(), params[CVDEPTH_BASE + RESO_PARAM].getValue(),   inputs[RESO_CV],   0.f, 1.f);
        const bool  rev = params[REVERSE_PARAM].getValue() >= 0.5f;
        const int   loopMode = (int)std::lround(params[LOOPMODE_PARAM].getValue());
        const int   trigMode = (int)std::lround(params[TRIGMODE_PARAM].getValue());

        const double fs = (double)n * (double)sStart;
        const double fe = (double)n * (double)sEnd;
        const double fls = std::max(fs, (double)n * (double)lStart);
        double fle = std::min(fe, (double)n * (double)lEnd);
        if (fle <= fls) fle = std::min(fe, fls + 1.0);

        // ---- gate ----------------------------------------------------------
        const float gv = inputs[GATE_INPUT].getVoltage();
        const bool hi = gv >= kGateHigh, lo = gv <= kGateLow;
        if (hi && !gateHigh_) {
            gateHigh_ = true;
            pos_ = rev ? fe - 1.0 : fs;
            dir_ = rev ? -1 : 1;
            playing_ = true; stage_ = 1;
        } else if (lo && gateHigh_) {
            gateHigh_ = false;
            if (trigMode == TRIG_GATE) stage_ = 4;      // release
        }

        // ---- amp envelope ---------------------------------------------------
        auto secs = [](float knob) { return 0.001f + knob * knob * 4.f; };  // 1ms..4s
        if (stage_ == 1) {                                   // attack
            env_ += args.sampleTime / secs(aT);
            if (env_ >= 1.f) { env_ = 1.f; stage_ = 2; }
        } else if (stage_ == 2) {                            // decay
            env_ -= args.sampleTime / secs(dT) * (1.f - sL);
            if (env_ <= sL) { env_ = sL; stage_ = 3; }
        } else if (stage_ == 4) {                            // release
            env_ -= args.sampleTime / secs(rT);
            if (env_ <= 0.f) { env_ = 0.f; stage_ = 0; playing_ = false; }
        }

        // ---- granular ---------------------------------------------------------
        const bool granular = params[GRAIN_PARAM].getValue() >= 0.5f;
        float l = 0.f, r = 0.f;
        bool cycled = false;

        if (granular) {
            const float gSize   = cvSum(params[GSIZE_PARAM].getValue(), params[CVDEPTH_BASE + GSIZE_PARAM].getValue(),   inputs[GSIZE_CV],   0.f, 1.f);
            const float gDens   = cvSum(params[GDENS_PARAM].getValue(), params[CVDEPTH_BASE + GDENS_PARAM].getValue(),   inputs[GDENS_CV],   0.f, 1.f);
            const float gSpray  = cvSum(params[GSPRAY_PARAM].getValue(), params[CVDEPTH_BASE + GSPRAY_PARAM].getValue(),  inputs[GSPRAY_CV],  0.f, 1.f);
            const float gJit    = cvSum(params[GJITTER_PARAM].getValue(), params[CVDEPTH_BASE + GJITTER_PARAM].getValue(), inputs[GJITTER_CV], 0.f, 1.f);
            const float gShape  = params[GSHAPE_PARAM].getValue();
            const float gSpread = params[GSPREAD_PARAM].getValue();
            const float gRevP   = params[GREVP_PARAM].getValue();

            // size 2ms..500ms, density 1..80 grains/sec
            const double sizeFrames = (0.002 + (double)gSize * 0.498) * args.sampleRate;
            const double perSec     = 1.0 + (double)gDens * 79.0;
            const double interval   = args.sampleRate / perSec;

            const float semis = tune + fine + inputs[PITCH_INPUT].getVoltage() * 12.f;
            const double ratio = (clipRate_ / (double)args.sampleRate);

            if (playing_) {
                // The playhead still walks the region; grains are sprayed around
                // it, so START/END and the loop still shape where the cloud sits.
                const double baseRate = std::pow(2.0, (double)semis / 12.0) * ratio;
                pos_ += baseRate * (double)dir_;
                if (loopMode != LOOP_OFF) {
                    if (pos_ >= fle) { pos_ = fls + (pos_ - fle); cycled = true; }
                } else if (pos_ >= fe) { pos_ = fe; cycled = true; }

                grainClock_ -= 1.0;
                if (grainClock_ <= 0.0) {
                    grainClock_ += interval;
                    for (int g = 0; g < kMaxGrains; ++g) {
                        if (grains_[g].on) continue;
                        Grain& gr = grains_[g];
                        const double spray = ((double)frand() * 2.0 - 1.0)
                                           * (double)gSpray * (fe - fs) * 0.5;
                        gr.pos  = rack::clamp((float)(pos_ + spray), (float)fs, (float)fe);
                        const float jit = (frand() * 2.f - 1.f) * gJit * 12.f;  // +/- an octave
                        gr.rate = std::pow(2.0, (double)(semis + jit) / 12.0) * ratio;
                        gr.dir  = (frand() < gRevP) ? -1 : 1;
                        gr.life = (int)sizeFrames;
                        gr.age  = 0;
                        const float p = 0.5f + (frand() - 0.5f) * gSpread;
                        gr.panL = std::cos(p * 1.57079633f);
                        gr.panR = std::sin(p * 1.57079633f);
                        gr.on   = true;
                        break;
                    }
                }
            }

            for (int g = 0; g < kMaxGrains; ++g) {
                Grain& gr = grains_[g];
                if (!gr.on) continue;
                float gl = 0.f, grr = 0.f;
                readAt(gr.pos, gl, grr);
                // Window: shape morphs a raised-cosine into a near-rectangular
                // gate, which is the difference between a smooth cloud and a
                // percussive one.
                const float t = (float)gr.age / (float)std::max(1, gr.life);
                const float raised = 0.5f - 0.5f * std::cos(t * 6.28318531f);
                const float w = std::pow(raised, 0.25f + (1.f - gShape) * 3.f);
                l += gl * w * gr.panL;
                r += grr * w * gr.panR;
                gr.pos += gr.rate * (double)gr.dir;
                if (++gr.age >= gr.life || gr.pos < 0.0 || gr.pos >= (double)n)
                    gr.on = false;
            }
            // Overlapping grains sum, so scale by the expected overlap or dense
            // settings clip instantly.
            const float overlap = (float)std::max(1.0, sizeFrames / std::max(1.0, interval));
            const float norm = 1.f / std::sqrt(overlap);
            l *= norm; r *= norm;
        }

        // ---- advance --------------------------------------------------------
        if (!granular && playing_) {
            readAt(pos_, l, r);

            // NON-DESTRUCTIVE seamless loop.  Approaching the loop end we fade
            // into the audio that precedes the loop START, so by the time the
            // playhead wraps the signal already IS that audio and the splice is
            // continuous -- without altering a single stored sample.
            const float xfN = params[LOOPXFADE_PARAM].getValue();
            if (loopMode != LOOP_OFF && xfN > 0.f && dir_ > 0) {
                double xf = (double)xfN * (double)n;
                xf = std::min(xf, fle - fls);          // never longer than the loop
                xf = std::min(xf, fls);                // needs run-in before it
                if (xf > 1.0 && pos_ > fle - xf) {
                    const double t = (pos_ - (fle - xf)) / xf;      // 0..1
                    float sl = 0.f, sr = 0.f;
                    readAt(fls - xf + t * xf, sl, sr);
                    const float a = std::cos((float)t * 1.57079633f);
                    const float b = std::sin((float)t * 1.57079633f);
                    l = l * a + sl * b;
                    r = r * a + sr * b;
                }
            }

            const float semis = tune + fine + inputs[PITCH_INPUT].getVoltage() * 12.f;
            const double ratio = (clipRate_ / (double)args.sampleRate);
            const double rate = std::pow(2.0, (double)semis / 12.0) * ratio;
            pos_ += rate * (double)dir_;
            // Cached so a mutex-contended block can keep the playhead moving.
            heldRate_ = rate * (double)dir_;
            heldFs_ = fs; heldFe_ = fe; heldFls_ = fls; heldFle_ = fle;
            heldLoopMode_ = loopMode;

            if (loopMode == LOOP_PINGPONG) {
                if (pos_ >= fle) { pos_ = fle; dir_ = -1; cycled = true; }
                else if (pos_ <= fls) { pos_ = fls; dir_ = 1; cycled = true; }
            } else if (loopMode == LOOP_FWD) {
                if (dir_ > 0 && pos_ >= fle) { pos_ = fls + (pos_ - fle); cycled = true; }
                else if (dir_ < 0 && pos_ <= fls) { pos_ = fle - (fls - pos_); cycled = true; }
            } else {                                          // one-shot
                if ((dir_ > 0 && pos_ >= fe) || (dir_ < 0 && pos_ <= fs)) {
                    cycled = true;
                    if (stage_ != 4) { stage_ = 4; }           // fall into release
                    pos_ = rack::clamp((float)pos_, (float)fs, (float)fe);
                }
            }
        }
        if (granular && !playing_) {
            for (int g = 0; g < kMaxGrains; ++g) grains_[g].on = false;
        }
        if (cycled) eoc_ = 1e-3f;

        // ---- filter (state-variable low-pass) --------------------------------
        if (cut < 0.999f) {
            // CLAMP AGAINST NYQUIST, as VCF does.  20*1000^cut reaches ~20 kHz
            // regardless of rate, and g = tan(pi*fc/sr) blows up as fc nears
            // sr/2: 40 kHz gave tan(pi/2) = inf and 32 kHz gave g = -2.61, i.e.
            // a filter with a NEGATIVE cutoff.  With no non-finite guard on the
            // state, the resulting NaN latched and the module went silent until
            // it was reset.
            const float nyq = args.sampleRate * 0.49f;
            const float fc = std::min(20.f * std::pow(1000.f, cut), nyq);
            const float g = std::tan((float)M_PI * fc * args.sampleTime);
            const float k = 2.f - 1.9f * res;                          // damping
            const float a1 = 1.f / (1.f + g * (g + k));
            // TWO filters, one per channel.  The old code ran ONE mono SVF and
            // applied its output as a per-channel CORRECTION (l += lp - mono),
            // which is algebraically l' = (l-r)/2 + lp: with the filter shut the
            // entire SIDE signal survived at full amplitude and the lowpass did
            // nothing at all to stereo material.
            const float hpL = (l - (g + k) * bpL_ - lpL_) * a1;
            bpL_ += g * hpL; lpL_ += g * bpL_;
            const float hpR = (r - (g + k) * bpR_ - lpR_) * a1;
            bpR_ += g * hpR; lpR_ += g * bpR_;
            if (!std::isfinite(lpL_) || !std::isfinite(bpL_) ||
                !std::isfinite(lpR_) || !std::isfinite(bpR_)) {
                lpL_ = bpL_ = lpR_ = bpR_ = 0.f;    // never let a NaN latch
            }
            l = lpL_; r = lpR_;
        }

        // ---- level / pan -----------------------------------------------------
        const float amp = env_ * level;
        const float pl = std::cos((pan + 1.f) * 0.25f * (float)M_PI);
        const float pr = std::sin((pan + 1.f) * 0.25f * (float)M_PI);
        lastL_ = l * amp * pl * 5.f;
        lastR_ = r * amp * pr * 5.f;
        outputs[LEFT_OUTPUT].setVoltage(lastL_);
        outputs[RIGHT_OUTPUT].setVoltage(lastR_);
        outputs[LEFT_OUTPUT].channels = outputs[RIGHT_OUTPUT].channels = 1;

        if (eoc_ > 0.f) eoc_ -= args.sampleTime;
        outputs[EOC_OUTPUT].setVoltage(eoc_ > 0.f ? 10.f : 0.f);
        lights[PLAY_LIGHT].setBrightnessRGB(env_ * 0.2f, env_ * 0.9f, env_);
    }
};

rackx::PanelElement el(int id, float x, float y, float radius,
                       rackx::PanelControlStyle style, const std::string& label) {
    rackx::PanelElement v;
    v.id = id; v.x = x; v.y = y; v.radius = radius;
    v.style = style; v.label = label;
    v.labelPlacement = rackx::PanelLabelPlacement::Below;
    return v;
}

rackx::PanelElement numbox(int id, float x, float y, const std::string& label) {
    rackx::PanelElement v = el(id, x, y, 11.f, rackx::PanelControlStyle::SegmentDisplay, label);
    v.width = 52.f; v.height = 22.f;
    return v;
}

// Section decor is drawn CENTRED on x,y; take top-left and convert.
rackx::PanelElement sect(float x, float y, float w, float h, const std::string& label) {
    rackx::PanelElement v;
    v.style = rackx::PanelControlStyle::Section;
    v.x = x + w * 0.5f; v.y = y + h * 0.5f;
    v.width = w; v.height = h;
    v.label = label;
    v.labelPlacement = rackx::PanelLabelPlacement::Above;
    return v;
}

rackx::PanelSpec samplerPanel() {
    using S = rackx::PanelControlStyle;
    // Big faceplate: a full-width waveform canvas on top, then four captioned
    // control rows.  ~2x the standard panel height and 40 HP wide.
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(40);
    panel.height = 880.f;
    panel.headerHeight = 24.f;
    const float W = panel.width;

    // ---- inline sample canvas -------------------------------------------
    {
        rackx::PanelElement wave;
        wave.id = Sampler1::WAVE_UI_PARAM;      // unique real param id
        wave.style = S::Waveform;
        wave.x = W * 0.5f; wave.y = 172.f;
        wave.width = W - 30.f; wave.height = 250.f;
        wave.labelPlacement = rackx::PanelLabelPlacement::None;
        panel.params.push_back(wave);
    }

    // Every knob sits directly above its own CV jack, so the pairing is obvious
    // at a glance rather than needing the labels to be read.
    // The house control unit: knob (inner value / outer CV depth), its jack,
    // then a segment display naming it -- see rack_panel_kit.h.
    auto pair = [&](int pid, int cvid, float x, float y, const std::string& lab,
                    float r = 13.f) {
        (void)r;
        rackx::kit::addControl(panel, pid, Sampler1::CVDEPTH_BASE + pid, cvid,
                               Sampler1::READOUT_BASE + pid, x, y, lab);
    };

    const float c4 = (W - 30.f) / 4.f;
    auto col4 = [&](int i) { return 15.f + c4 * ((float)i + 0.5f); };

    panel.decor.push_back(sect(15.f, 316.f, W - 30.f, 116.f, "PITCH / REGION"));
    pair(Sampler1::TUNE_PARAM,  Sampler1::TUNE_CV,  col4(0), 354.f, "TUNE", 15.f);
    pair(Sampler1::FINE_PARAM,  Sampler1::FINE_CV,  col4(1), 354.f, "FINE");
    pair(Sampler1::START_PARAM, Sampler1::START_CV, col4(2), 354.f, "START");
    pair(Sampler1::END_PARAM,   Sampler1::END_CV,   col4(3), 354.f, "END");

    panel.decor.push_back(sect(15.f, 442.f, W - 30.f, 116.f, "LOOP"));
    panel.params.push_back(numbox(Sampler1::LOOPMODE_PARAM, col4(0), 480.f, "MODE"));
    pair(Sampler1::LOOPSTART_PARAM, Sampler1::LOOPSTART_CV, col4(1), 480.f, "L START");
    pair(Sampler1::LOOPEND_PARAM,   Sampler1::LOOPEND_CV,   col4(2), 480.f, "L END");
    panel.params.push_back(el(Sampler1::REVERSE_PARAM, col4(3), 480.f, 10.f, S::Switch, "REVERSE"));

    panel.decor.push_back(sect(15.f, 568.f, W - 30.f, 116.f, "AMP ENVELOPE"));
    pair(Sampler1::ATTACK_PARAM,  Sampler1::ATTACK_CV,  col4(0), 606.f, "ATT");
    pair(Sampler1::DECAY_PARAM,   Sampler1::DECAY_CV,   col4(1), 606.f, "DEC");
    pair(Sampler1::SUSTAIN_PARAM, Sampler1::SUSTAIN_CV, col4(2), 606.f, "SUS");
    pair(Sampler1::RELEASE_PARAM, Sampler1::RELEASE_CV, col4(3), 606.f, "REL");

    panel.decor.push_back(sect(15.f, 694.f, W - 30.f, 116.f, "FILTER / OUT"));
    pair(Sampler1::CUTOFF_PARAM, Sampler1::CUTOFF_CV, col4(0), 732.f, "CUTOFF");
    pair(Sampler1::RESO_PARAM,   Sampler1::RESO_CV,   col4(1), 732.f, "RESO");
    pair(Sampler1::LEVEL_PARAM,  Sampler1::LEVEL_CV,  col4(2), 732.f, "LEVEL");
    pair(Sampler1::PAN_PARAM,    Sampler1::PAN_CV,    col4(3), 732.f, "PAN");

    // ---- granular row ----------------------------------------------------
    panel.decor.push_back(sect(15.f, 694.f, W - 30.f, 116.f, "GRANULAR"));
    {
        const float c8 = (W - 30.f) / 8.f;
        auto g8 = [&](int i) { return 15.f + c8 * ((float)i + 0.5f); };
        panel.params.push_back(el(Sampler1::GRAIN_PARAM, g8(0), 732.f, 10.f, S::Switch, "GRAIN"));
        pair(Sampler1::GSIZE_PARAM,   Sampler1::GSIZE_CV,   g8(1), 732.f, "SIZE");
        pair(Sampler1::GDENS_PARAM,   Sampler1::GDENS_CV,   g8(2), 732.f, "DENS");
        pair(Sampler1::GSPRAY_PARAM,  Sampler1::GSPRAY_CV,  g8(3), 732.f, "SPRAY");
        pair(Sampler1::GJITTER_PARAM, Sampler1::GJITTER_CV, g8(4), 732.f, "JITTER");
        panel.params.push_back(el(Sampler1::GSHAPE_PARAM,  g8(5), 732.f, 13.f, S::Knob, "SHAPE"));
        panel.params.push_back(el(Sampler1::GSPREAD_PARAM, g8(6), 732.f, 13.f, S::Knob, "SPREAD"));
        panel.params.push_back(el(Sampler1::GREVP_PARAM,   g8(7), 732.f, 13.f, S::Knob, "REV%"));
    }

    // I/O strip
    panel.decor.push_back(sect(15.f, 820.f, W - 30.f, 40.f, "I / O"));
    const float jy = 842.f;
    panel.inputs.push_back(el(Sampler1::PITCH_INPUT, 40.f,  jy, 8.f, S::Knob, "V/OCT"));
    panel.inputs.push_back(el(Sampler1::GATE_INPUT,  95.f,  jy, 8.f, S::Knob, "GATE"));
    panel.params.push_back(numbox(Sampler1::TRIGMODE_PARAM, 165.f, jy, "TRIG"));
    panel.outputs.push_back(el(Sampler1::LEFT_OUTPUT,  W - 130.f, jy, 8.f, S::Knob, "L"));
    panel.outputs.push_back(el(Sampler1::RIGHT_OUTPUT, W -  90.f, jy, 8.f, S::Knob, "R"));
    panel.outputs.push_back(el(Sampler1::EOC_OUTPUT,   W -  45.f, jy, 8.f, S::Knob, "EOC"));
    panel.lights.push_back(el(Sampler1::PLAY_LIGHT, 128.f, jy, 4.f, S::Lamp, ""));
    panel.lights.push_back(el(Sampler1::LOAD_LIGHT, 145.f, jy, 4.f, S::Lamp, ""));
    return panel;
}

} // namespace

namespace rackx {
void registerSamplerModule() {
    addType("SMPL", "Sampler", "Sampler", Role::Normal,
            [] { return std::make_unique<Sampler1>(); }, samplerPanel());
}
} // namespace rackx
