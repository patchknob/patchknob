//----------------------------------------------------------------------------
//  rack_zplane.cpp -- ZPLANE: an E-mu style morphing Z-plane filter.
//
//  WHAT THE REAL THING IS.  The E-mu Emulator IV / Morpheus / Ultra Proteus
//  "Z-plane" filter is not a ladder with a cutoff knob.  It is a CASCADE OF SIX
//  BIQUADS -- a 12th-order section -- whose coefficients are not computed from a
//  formula but STORED, as a handful of snapshots ("frames") of where the poles
//  and zeros sit in the z plane.  A preset (E-mu called them cubes) is a path
//  through those frames, and the morph control interpolates along it, dragging
//  the poles and zeros through the plane.  That is where the name comes from,
//  and why it can do things a cutoff-and-resonance filter cannot: a frame can
//  hold five independent formants and the morph can glide one vowel into
//  another without any of them passing through "a lowpass".
//
//  WHY THIS INTERPOLATES FREQUENCY/Q AND NOT COEFFICIENTS.  The obvious way to
//  morph is to lerp the biquad coefficients of frame A toward frame B.  That is
//  wrong twice over.  Musically, a coefficient lerp does not move a resonance
//  from 700 Hz to 300 Hz -- it fades the first one out while the second fades
//  in, so a vowel morph sounds like a crossfade instead of a glide.  Worse, the
//  stable region of (a1,a2) space is a TRIANGLE, and the straight line between
//  two stable frames can leave it: a lerp of two perfectly good filters can be
//  an oscillator.  So frames here are stored as what they physically are --
//  centre frequency, Q and gain per section -- and the interpolation happens in
//  that space.  Poles are rebuilt from the interpolated values afterwards, so
//  every intermediate point is a real filter with r < 1 by construction.  It is
//  both stabler and more musical than what the hardware could afford to do.
//
//  STABILITY, concretely: r = exp(-pi*f/(Q*sr)) is < 1 for every f > 0, Q > 0,
//  and is additionally clamped to 0.9995; theta = 2*pi*f/sr is clamped below pi
//  so a section can never wrap past Nyquist and alias its own resonance down.
//  Each section is normalised to unity at its own centre frequency, so morphing
//  and sweeping do not change the output level.  A non-finite sample resets that
//  voice's state instead of letting a NaN into the mix.
//
//  CONTROLS.  TRANSFORM shifts every section's frequency together (the closest
//  thing this structure has to a cutoff, and what the hardware's "transform 2"
//  did); MORPH walks the preset's frames; RESO scales every section's Q at once.
//  Those three are the instrument.  The rest -- drive, key tracking, dry/wet,
//  level -- are the usual trimmings.
//
//  Every continuous control is a CONCENTRIC KnobCV: outer ring = CV depth
//  (bipolar attenuverter), inner disc = base value, one jack beneath.
//----------------------------------------------------------------------------
#include "rack_factory.h"
#include "rack_panel_kit.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using rack::engine::Module;

constexpr float kPi        = 3.14159265358979323846f;
constexpr int   kMaxVoices = rack::engine::PORT_MAX_CHANNELS;   // 16
constexpr int   kSections  = 6;    // 6 biquads == 12th order, as per E-mu
constexpr int   kMaxFrames = 4;    // frames per preset (the "cube" corners)
constexpr int   kCtrlDiv   = 16;   // recompute coefficients every N samples

// --------------------------------------------------------------------------
//  Frame data
// --------------------------------------------------------------------------
// How a section places its ZEROS.  The poles give the resonance; the zeros
// decide what the section does with everything else, and that is most of a
// preset's character.  Band is a pure resonator (zeros at DC and Nyquist), the
// others shape the skirt.
enum ZeroMode : unsigned char { Z_BAND, Z_LOW, Z_HIGH, Z_NOTCH };

struct ZSection {
    float     freq;    //!< centre frequency in Hz at the preset's nominal pitch
    float     q;       //!< resonance sharpness; higher == narrower and louder
    float     gainDb;  //!< section output trim
    ZeroMode  zero;
};

struct ZFrame { ZSection s[kSections]; };

struct ZPreset {
    const char* name;
    int         frames;
    ZFrame      f[kMaxFrames];
};

// Formant data below follows the standard measured values for sung vowels
// (Peterson & Barney's F1/F2/F3 averages), which is why the vowel morphs read
// as speech rather than as "a filter sweep with a lot of resonance".
const ZPreset kPresets[] = {

    // ---- vowels -----------------------------------------------------------
    { "Vowel Ah-Ee", 3, {
      // /a/ 730 1090 2440       /e/ 530 1840 2480        /i/ 270 2290 3010
      {{{ 730, 8,  4, Z_BAND},{1090, 10, 3, Z_BAND},{2440, 12, 1, Z_BAND},
        {3400, 12, -2, Z_BAND},{4500, 10, -6, Z_BAND},{ 180, 2, -3, Z_HIGH}}},
      {{{ 530, 9,  4, Z_BAND},{1840, 11, 3, Z_BAND},{2480, 12, 1, Z_BAND},
        {3500, 12, -2, Z_BAND},{4600, 10, -6, Z_BAND},{ 180, 2, -3, Z_HIGH}}},
      {{{ 270, 10, 4, Z_BAND},{2290, 12, 4, Z_BAND},{3010, 13, 2, Z_BAND},
        {3700, 12, -2, Z_BAND},{4700, 10, -6, Z_BAND},{ 180, 2, -3, Z_HIGH}}} } },

    { "Vowel Oo-Ah", 3, {
      // /u/ 300 870 2240        /o/ 570 840 2410         /a/ 730 1090 2440
      {{{ 300, 10, 5, Z_BAND},{ 870, 11, 2, Z_BAND},{2240, 12, -2, Z_BAND},
        {3200, 12, -6, Z_BAND},{4200, 10, -9, Z_BAND},{ 160, 2, -3, Z_HIGH}}},
      {{{ 570, 9,  5, Z_BAND},{ 840, 10, 3, Z_BAND},{2410, 12, -1, Z_BAND},
        {3300, 12, -5, Z_BAND},{4300, 10, -8, Z_BAND},{ 160, 2, -3, Z_HIGH}}},
      {{{ 730, 8,  4, Z_BAND},{1090, 10, 3, Z_BAND},{2440, 12,  1, Z_BAND},
        {3400, 12, -2, Z_BAND},{4500, 10, -6, Z_BAND},{ 160, 2, -3, Z_HIGH}}} } },

    { "Vowel Ee-Oh", 3, {
      {{{ 270, 10, 4, Z_BAND},{2290, 12, 4, Z_BAND},{3010, 13,  2, Z_BAND},
        {3700, 12, -2, Z_BAND},{4700, 10, -6, Z_BAND},{ 180, 2, -3, Z_HIGH}}},
      {{{ 400, 9,  4, Z_BAND},{1700, 11, 3, Z_BAND},{2600, 12,  0, Z_BAND},
        {3500, 12, -3, Z_BAND},{4500, 10, -7, Z_BAND},{ 180, 2, -3, Z_HIGH}}},
      {{{ 570, 9,  5, Z_BAND},{ 840, 10, 3, Z_BAND},{2410, 12, -1, Z_BAND},
        {3300, 12, -5, Z_BAND},{4300, 10, -8, Z_BAND},{ 180, 2, -3, Z_HIGH}}} } },

    // ---- classic sweeps ---------------------------------------------------
    // Six lowpasses stacked with rising Q: a 12-pole slope whose corner is set
    // by TRANSFORM.  Morph tilts it from gentle to a hard resonant knee.
    { "Sweep 12-Pole", 3, {
      {{{ 800, 0.7f, 0, Z_LOW},{ 800, 0.7f, 0, Z_LOW},{ 800, 0.7f, 0, Z_LOW},
        { 800, 0.7f, 0, Z_LOW},{ 800, 0.7f, 0, Z_LOW},{ 800, 0.7f, 0, Z_LOW}}},
      {{{ 800, 1.2f, 0, Z_LOW},{ 800, 1.0f, 0, Z_LOW},{ 800, 0.8f, 0, Z_LOW},
        { 800, 0.7f, 0, Z_LOW},{ 800, 0.7f, 0, Z_LOW},{ 800, 0.7f, 0, Z_LOW}}},
      {{{ 800, 6.0f, 3, Z_LOW},{ 800, 2.0f, 0, Z_LOW},{ 800, 1.0f, 0, Z_LOW},
        { 800, 0.7f, 0, Z_LOW},{ 800, 0.7f, 0, Z_LOW},{ 800, 0.7f, 0, Z_LOW}}} } },

    { "Sweep Band", 3, {
      {{{ 400, 3, 3, Z_BAND},{ 800, 3, 2, Z_BAND},{1600, 3, 1, Z_BAND},
        {3200, 3, 0, Z_BAND},{6400, 3, -1, Z_BAND},{ 100, 1, -6, Z_HIGH}}},
      {{{ 700, 6, 4, Z_BAND},{1400, 6, 2, Z_BAND},{2800, 6, 0, Z_BAND},
        {5600, 6, -2, Z_BAND},{9000, 5, -4, Z_BAND},{ 100, 1, -6, Z_HIGH}}},
      {{{1200, 12, 5, Z_BAND},{2400, 12, 2, Z_BAND},{4800, 10, -1, Z_BAND},
        {7200, 8, -4, Z_BAND},{9600, 6, -7, Z_BAND},{ 100, 1, -6, Z_HIGH}}} } },

    // ---- resonant character -----------------------------------------------
    // Harmonically spaced peaks: the comb-like metallic ring the Morpheus was
    // known for.  Morph slides the series from harmonic to inharmonic, which is
    // what turns "chorus of resonances" into "struck metal".
    { "Metal Comb", 3, {
      {{{ 300, 14, 3, Z_BAND},{ 600, 14, 2, Z_BAND},{ 900, 14, 2, Z_BAND},
        {1200, 14, 1, Z_BAND},{1500, 14, 1, Z_BAND},{1800, 14, 0, Z_BAND}}},
      {{{ 300, 18, 3, Z_BAND},{ 720, 18, 2, Z_BAND},{1180, 18, 2, Z_BAND},
        {1710, 18, 1, Z_BAND},{2290, 18, 1, Z_BAND},{2930, 18, 0, Z_BAND}}},
      {{{ 300, 24, 4, Z_BAND},{ 830, 24, 3, Z_BAND},{1520, 24, 2, Z_BAND},
        {2350, 24, 1, Z_BAND},{3300, 24, 0, Z_BAND},{4360, 24, -1, Z_BAND}}} } },

    { "Bell Glass", 3, {
      // Inharmonic partials in the ratios a struck bar actually produces.
      {{{ 500, 20, 3, Z_BAND},{1350, 20, 2, Z_BAND},{2650, 20, 1, Z_BAND},
        {4300, 20, 0, Z_BAND},{6200, 18, -2, Z_BAND},{ 120, 1, -6, Z_HIGH}}},
      {{{ 500, 30, 4, Z_BAND},{1385, 30, 3, Z_BAND},{2720, 30, 2, Z_BAND},
        {4490, 30, 1, Z_BAND},{6600, 26, -1, Z_BAND},{ 120, 1, -6, Z_HIGH}}},
      {{{ 500, 44, 5, Z_BAND},{1420, 44, 4, Z_BAND},{2800, 44, 3, Z_BAND},
        {4700, 44, 2, Z_BAND},{7000, 36, 0, Z_BAND},{ 120, 1, -6, Z_HIGH}}} } },

    { "Throat Nasal", 3, {
      // A nasal tract adds a ZERO between the low formants -- that antiresonance
      // is the whole difference between "ah" and "ng", so section 3 is a notch.
      {{{ 320, 9, 4, Z_BAND},{1200, 10, 2, Z_BAND},{1000, 6, -8, Z_NOTCH},
        {2600, 11, 0, Z_BAND},{3600, 10, -4, Z_BAND},{ 200, 2, -3, Z_HIGH}}},
      {{{ 400, 9, 4, Z_BAND},{1000, 10, 1, Z_BAND},{1500, 8, -12, Z_NOTCH},
        {2400, 11, 0, Z_BAND},{3400, 10, -4, Z_BAND},{ 200, 2, -3, Z_HIGH}}},
      {{{ 250, 10, 5, Z_BAND},{ 900, 12, 0, Z_BAND},{2000, 10, -16, Z_NOTCH},
        {2800, 12, 1, Z_BAND},{3800, 10, -4, Z_BAND},{ 200, 2, -3, Z_HIGH}}} } },

    { "Phaser Notch", 3, {
      // Six notches marching upward: a fixed phaser you can morph rather than
      // sweep, so the notches move without the LFO's periodicity.
      {{{ 300, 4, 0, Z_NOTCH},{ 600, 4, 0, Z_NOTCH},{1200, 4, 0, Z_NOTCH},
        {2400, 4, 0, Z_NOTCH},{4800, 4, 0, Z_NOTCH},{9600, 4, 0, Z_NOTCH}}},
      {{{ 450, 5, 0, Z_NOTCH},{ 900, 5, 0, Z_NOTCH},{1800, 5, 0, Z_NOTCH},
        {3600, 5, 0, Z_NOTCH},{7200, 5, 0, Z_NOTCH},{11000, 5, 0, Z_NOTCH}}},
      {{{ 700, 7, 0, Z_NOTCH},{1400, 7, 0, Z_NOTCH},{2800, 7, 0, Z_NOTCH},
        {5600, 7, 0, Z_NOTCH},{9000, 6, 0, Z_NOTCH},{13000, 5, 0, Z_NOTCH}}} } },

    { "Male-Female", 3, {
      // The same vowel with the whole formant set scaled: a shorter tract puts
      // every formant higher, which is the actual difference, not just pitch.
      {{{ 640, 8, 4, Z_BAND},{1190, 10, 3, Z_BAND},{2390, 12, 1, Z_BAND},
        {3300, 12, -2, Z_BAND},{4400, 10, -6, Z_BAND},{ 150, 2, -3, Z_HIGH}}},
      {{{ 730, 8, 4, Z_BAND},{1300, 10, 3, Z_BAND},{2600, 12, 1, Z_BAND},
        {3600, 12, -2, Z_BAND},{4800, 10, -6, Z_BAND},{ 170, 2, -3, Z_HIGH}}},
      {{{ 850, 8, 4, Z_BAND},{1500, 10, 3, Z_BAND},{2900, 12, 1, Z_BAND},
        {4000, 12, -2, Z_BAND},{5300, 10, -6, Z_BAND},{ 200, 2, -3, Z_HIGH}}} } },

    { "Wah Pedal", 3, {
      {{{ 450, 5, 6, Z_BAND},{ 900, 3, 1, Z_BAND},{2200, 2, -4, Z_LOW},
        {2200, 2, 0, Z_LOW},{ 120, 1, -4, Z_HIGH},{ 120, 1, 0, Z_HIGH}}},
      {{{ 900, 7, 7, Z_BAND},{1800, 3, 1, Z_BAND},{3000, 2, -3, Z_LOW},
        {3000, 2, 0, Z_LOW},{ 140, 1, -4, Z_HIGH},{ 140, 1, 0, Z_HIGH}}},
      {{{1800, 9, 8, Z_BAND},{3200, 3, 0, Z_BAND},{4200, 2, -3, Z_LOW},
        {4200, 2, 0, Z_LOW},{ 160, 1, -4, Z_HIGH},{ 160, 1, 0, Z_HIGH}}} } },

    { "Formant Choir", 4, {
      // Four vowels on one path, so a single morph sweep says "ah-eh-ee-oo".
      {{{ 730, 9, 4, Z_BAND},{1090, 11, 3, Z_BAND},{2440, 12, 1, Z_BAND},
        {3400, 12, -3, Z_BAND},{4500, 10, -7, Z_BAND},{ 180, 2, -3, Z_HIGH}}},
      {{{ 530, 9, 4, Z_BAND},{1840, 11, 3, Z_BAND},{2480, 12, 1, Z_BAND},
        {3500, 12, -3, Z_BAND},{4600, 10, -7, Z_BAND},{ 180, 2, -3, Z_HIGH}}},
      {{{ 270, 11, 4, Z_BAND},{2290, 13, 4, Z_BAND},{3010, 13, 2, Z_BAND},
        {3700, 12, -3, Z_BAND},{4700, 10, -7, Z_BAND},{ 180, 2, -3, Z_HIGH}}},
      {{{ 300, 11, 5, Z_BAND},{ 870, 12, 2, Z_BAND},{2240, 12, -2, Z_BAND},
        {3200, 12, -6, Z_BAND},{4200, 10, -9, Z_BAND},{ 180, 2, -3, Z_HIGH}}} } },
};
constexpr int kNumPresets = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

// --------------------------------------------------------------------------
//  One biquad, transposed direct form II (fewer state variables, and the form
//  that behaves best when coefficients are changed underneath a running filter
//  -- which is exactly what morphing does every control period).
// --------------------------------------------------------------------------
struct Biquad {
    float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
    float z1 = 0.f, z2 = 0.f;

    inline float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    inline void reset() { z1 = z2 = 0.f; }
};

//! Build one section from a physical description.  Poles come from (f,Q); the
//! zeros come from the mode; then the whole thing is normalised so the section
//! is unity at its own centre frequency -- without that, sweeping TRANSFORM or
//! turning up RESO would change the output level as well as the tone.
inline void makeSection(Biquad& bq, float freq, float q, float gainLin,
                        ZeroMode mode, float sr) {
    const float nyq = sr * 0.5f;
    freq = rack::clamp(freq, 10.f, nyq * 0.98f);
    q    = rack::clamp(q, 0.3f, 60.f);

    const float theta = 2.f * kPi * freq / sr;              // < pi by the clamp
    float r = std::exp(-kPi * freq / (q * sr));             // < 1 for all f,q > 0
    r = rack::clamp(r, 0.f, 0.9995f);                       // belt and braces

    bq.a1 = -2.f * r * std::cos(theta);
    bq.a2 = r * r;

    switch (mode) {
        case Z_LOW:   bq.b0 = 1.f; bq.b1 =  2.f; bq.b2 = 1.f; break;
        case Z_HIGH:  bq.b0 = 1.f; bq.b1 = -2.f; bq.b2 = 1.f; break;
        case Z_NOTCH: bq.b0 = 1.f; bq.b1 = -2.f * std::cos(theta); bq.b2 = 1.f; break;
        case Z_BAND:
        default:      bq.b0 = 1.f; bq.b1 =  0.f; bq.b2 = -1.f; break;
    }

    // Evaluate |H(e^jw)| at the section's own centre and scale to the wanted
    // gain.  A notch is zero there by definition, so it is normalised at DC
    // instead (where its passband lives).
    const float w  = (mode == Z_NOTCH) ? 0.f : theta;
    const float cw = std::cos(w), sw = std::sin(w);
    const float c2w = std::cos(2.f * w), s2w = std::sin(2.f * w);
    const float nr = bq.b0 + bq.b1 * cw + bq.b2 * c2w;
    const float ni = -(bq.b1 * sw + bq.b2 * s2w);
    const float dr = 1.f + bq.a1 * cw + bq.a2 * c2w;
    const float di = -(bq.a1 * sw + bq.a2 * s2w);
    const float nm = std::sqrt(nr * nr + ni * ni);
    const float dm = std::sqrt(dr * dr + di * di);
    float scale = (nm > 1e-9f) ? (gainLin * dm / nm) : gainLin;
    if (!std::isfinite(scale)) scale = gainLin;
    scale = rack::clamp(scale, 0.f, 64.f);

    bq.b0 *= scale; bq.b1 *= scale; bq.b2 *= scale;
}

inline float dbToLin(float db) { return std::pow(10.f, db * (1.f / 20.f)); }

// --------------------------------------------------------------------------

struct ZPlane final : Module {
    enum ParamIds {
        MORPH_PARAM, TRANSFORM_PARAM, RESO_PARAM, PRESET_PARAM,
        DRIVE_PARAM, KEYTRK_PARAM, MIX_PARAM, LEVEL_PARAM,
        // outer-ring CV depths, in the SAME order
        MORPH_CV_PARAM, TRANSFORM_CV_PARAM, RESO_CV_PARAM, PRESET_CV_PARAM,
        DRIVE_CV_PARAM, KEYTRK_CV_PARAM, MIX_CV_PARAM, LEVEL_CV_PARAM,
        // One inert readout param per control unit: a param can own only one
        // panel element, so the segment display needs a key of its own.
        READOUT_BASE,
        NUM_PARAMS = READOUT_BASE + 8
    };
    enum InputIds {
        AUDIO_INPUT, VOCT_INPUT,
        MORPH_INPUT, TRANSFORM_INPUT, RESO_INPUT, PRESET_INPUT,
        DRIVE_INPUT, KEYTRK_INPUT, MIX_INPUT, LEVEL_INPUT,
        NUM_INPUTS
    };
    enum OutputIds { MAIN_OUTPUT, NUM_OUTPUTS };
    enum LightIds  { RESO_LIGHT, DRIVE_LIGHT, NUM_LIGHTS };

    Biquad bq[kMaxVoices][kSections];
    int    ctrl[kMaxVoices] = {};        // control-rate divider phase, per voice

    //! RackEngine::resetModule() calls this, so without it "reset module" was
    //! silently a no-op here: six biquads per voice kept their z1/z2 and the
    //! previous patch's ringing carried straight across a patch load.  Zeroing
    //! the control-rate dividers too makes the next sample rebuild coefficients
    //! rather than run for up to kCtrlDiv samples on the old ones.
    void onReset() override {
        for (int v = 0; v < kMaxVoices; ++v) {
            for (int s = 0; s < kSections; ++s) bq[v][s].reset();
            ctrl[v] = 0;
        }
    }

    ZPlane() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(MORPH_PARAM,     0.f, 1.f, 0.f,  "Morph");
        configParam(TRANSFORM_PARAM, 0.f, 1.f, 0.5f, "Transform");
        configParam(RESO_PARAM,      0.f, 1.f, 0.5f, "Resonance");
        configParam(PRESET_PARAM,    0.f, (float)(kNumPresets - 1), 0.f, "Preset");
        configParam(DRIVE_PARAM,     0.f, 1.f, 0.f,  "Drive");
        configParam(KEYTRK_PARAM,    0.f, 1.f, 0.f,  "Key track");
        configParam(MIX_PARAM,       0.f, 1.f, 1.f,  "Dry / wet");
        configParam(LEVEL_PARAM,     0.f, 2.f, 1.f,  "Level");
        for (int i = MORPH_CV_PARAM; i <= LEVEL_CV_PARAM; ++i)
            configParam(i, -1.f, 1.f, 0.f, "CV depth");
    }

    inline float cv(int baseId, int cvId, int inId, int voice, float lo, float hi) const {
        float v = params[baseId].getValue();
        const rack::engine::Input& in = inputs[inId];
        if (in.isConnected())
            v += params[cvId].getValue() * in.getPolyVoltage(voice) * 0.1f * (hi - lo);
        return rack::clamp(v, lo, hi);
    }

    //! Interpolate the preset's frames at `morph` (0..1 across the whole path)
    //! in FREQUENCY/Q/GAIN space -- see the header for why not in coefficients.
    static void frameAt(const ZPreset& p, float morph, ZSection out[kSections]) {
        const int last = std::max(0, p.frames - 1);
        const float pos = rack::clamp(morph, 0.f, 1.f) * (float)last;
        const int   i0  = rack::clamp((int)pos, 0, last);
        const int   i1  = std::min(i0 + 1, last);
        const float t   = pos - (float)i0;
        const ZFrame& a = p.f[i0];
        const ZFrame& b = p.f[i1];
        for (int s = 0; s < kSections; ++s) {
            // Frequency glides GEOMETRICALLY (a semitone is a ratio, not a
            // number of hertz), which is what makes a formant sound like it
            // moves rather than fades.
            const float fa = std::max(1.f, a.s[s].freq), fb = std::max(1.f, b.s[s].freq);
            out[s].freq   = fa * std::pow(fb / fa, t);
            out[s].q      = a.s[s].q      + (b.s[s].q      - a.s[s].q)      * t;
            out[s].gainDb = a.s[s].gainDb + (b.s[s].gainDb - a.s[s].gainDb) * t;
            // A zero mode cannot be half-way, so it switches at the midpoint.
            out[s].zero   = (t < 0.5f) ? a.s[s].zero : b.s[s].zero;
        }
    }

    void process(const ProcessArgs& args) override {
        int voices = std::max(1, inputs[AUDIO_INPUT].getChannels());
        if (voices > kMaxVoices) voices = kMaxVoices;

        const float sr = args.sampleRate > 0.f ? args.sampleRate : 48000.f;
        float resoLit = 0.f, driveLit = 0.f;

        outputs[MAIN_OUTPUT].setChannels(voices);

        for (int v = 0; v < voices; ++v) {
            const float morph = cv(MORPH_PARAM,     MORPH_CV_PARAM,     MORPH_INPUT,     v, 0.f, 1.f);
            const float xform = cv(TRANSFORM_PARAM, TRANSFORM_CV_PARAM, TRANSFORM_INPUT, v, 0.f, 1.f);
            const float res01 = cv(RESO_PARAM,      RESO_CV_PARAM,      RESO_INPUT,      v, 0.f, 1.f);
            const float drv01 = cv(DRIVE_PARAM,     DRIVE_CV_PARAM,     DRIVE_INPUT,     v, 0.f, 1.f);
            const float ktrk  = cv(KEYTRK_PARAM,    KEYTRK_CV_PARAM,    KEYTRK_INPUT,    v, 0.f, 1.f);
            const float mix   = cv(MIX_PARAM,       MIX_CV_PARAM,       MIX_INPUT,       v, 0.f, 1.f);
            const float lvl   = cv(LEVEL_PARAM,     LEVEL_CV_PARAM,     LEVEL_INPUT,     v, 0.f, 2.f);
            const float psel  = cv(PRESET_PARAM,    PRESET_CV_PARAM,    PRESET_INPUT,    v,
                                   0.f, (float)(kNumPresets - 1));

            // Rebuilding six normalised biquads costs six transcendental calls
            // each; at audio rate that dwarfs the filtering itself, so it runs
            // on a control-rate divider.  Coefficients are stepped, not lerped:
            // transposed DF-II tolerates that without the click a direct-form
            // update produces.
            if (ctrl[v]-- <= 0) {
                ctrl[v] = kCtrlDiv;

                const int pi = rack::clamp((int)std::lround(psel), 0, kNumPresets - 1);
                ZSection sec[kSections];
                frameAt(kPresets[pi], morph, sec);

                // TRANSFORM shifts the whole set by +/- 2 octaves.  Scaling
                // every section by ONE ratio is what keeps a vowel a vowel while
                // it moves -- shifting them independently would just detune it.
                float ratio = std::pow(2.f, (xform - 0.5f) * 4.f);
                if (ktrk > 0.f && inputs[VOCT_INPUT].isConnected())
                    ratio *= std::pow(2.f, inputs[VOCT_INPUT].getPolyVoltage(v) * ktrk);

                // RESO scales Q about the preset's own value, so a preset that
                // is meant to be gentle stays gentle at noon.
                const float qScale = std::pow(8.f, (res01 - 0.5f) * 2.f);

                for (int s = 0; s < kSections; ++s)
                    makeSection(bq[v][s], sec[s].freq * ratio, sec[s].q * qScale,
                                dbToLin(sec[s].gainDb), sec[s].zero, sr);
            }

            // ---- audio ------------------------------------------------------
            const float dry = inputs[AUDIO_INPUT].getPolyVoltage(v);
            float x = dry;

            // Drive ahead of the filter, which is where it belongs: saturating
            // the INPUT feeds the resonances harmonics to grab onto, while
            // saturating the output would just clip the result.
            if (drv01 > 0.f) {
                const float g = 1.f + drv01 * 15.f;
                x = std::tanh(x * g * (1.f / 5.f)) * 5.f;
            }

            for (int s = 0; s < kSections; ++s) x = bq[v][s].process(x);

            // A non-finite sample would persist in the state forever; reset the
            // voice instead of letting it into the mix.
            if (!std::isfinite(x)) {
                for (int s = 0; s < kSections; ++s) bq[v][s].reset();
                x = 0.f;
            }

            float y = dry * (1.f - mix) + x * mix;
            y *= lvl;
            y = rack::clamp(y, -12.f, 12.f);      // headroom, not a limiter
            outputs[MAIN_OUTPUT].setVoltage(y, v);

            if (v == 0) {
                resoLit  = res01;
                driveLit = drv01;
            }
        }

        lights[RESO_LIGHT].setBrightness(resoLit);
        lights[DRIVE_LIGHT].setBrightness(driveLit);
    }
};

// ---------------------------------------------------------------------------
//  Panel
// ---------------------------------------------------------------------------
rackx::PanelElement el(int id, float x, float y, float r,
                       rackx::PanelControlStyle style, const std::string& label) {
    rackx::PanelElement v;
    v.id = id; v.x = x; v.y = y; v.radius = r; v.style = style; v.label = label;
    v.labelPlacement = rackx::PanelLabelPlacement::Below;
    return v;
}

int g_readout = 0;
void cvKnob(rackx::PanelSpec& p, int baseId, int cvId, int inId,
            float x, float y, const std::string& label, float cellW = 0.f) {
    rackx::kit::addControl(p, baseId, cvId, inId,
                           ZPlane::READOUT_BASE + g_readout++, x, y, label, cellW);
}

rackx::PanelElement section(float x, float y, float w, float h, const std::string& label) {
    rackx::PanelElement v;
    v.style = rackx::PanelControlStyle::Section;
    // Section decor is drawn CENTRED on x,y, so hand it the centre rather than
    // the top-left corner.
    v.x = x + w * 0.5f; v.y = y + h * 0.5f;
    v.width = w; v.height = h; v.label = label;
    v.labelPlacement = rackx::PanelLabelPlacement::Above;
    return v;
}

rackx::PanelSpec zplanePanel() {
    g_readout = 0;
    using S = rackx::PanelControlStyle;
    rackx::PanelSpec panel = rackx::PanelSpec::fromHp(22);
    panel.height = 400.f;
    panel.headerHeight = 24.f;
    const float W = panel.width;

    auto colx  = [&](int n, int i) { return 28.f + (W - 56.f) / (float)n * ((float)i + 0.5f); };
    auto cellw = [&](int n) { return (W - 56.f) / (float)n; };

    // The three controls that ARE the instrument get their own row and the most
    // room; everything else is support.
    panel.decor.push_back(section(20.f, 44.f, W - 40.f, 112.f, "Z-PLANE"));
    cvKnob(panel, ZPlane::MORPH_PARAM,     ZPlane::MORPH_CV_PARAM,     ZPlane::MORPH_INPUT,
           colx(3, 0), 74.f, "MORPH", cellw(3));
    cvKnob(panel, ZPlane::TRANSFORM_PARAM, ZPlane::TRANSFORM_CV_PARAM, ZPlane::TRANSFORM_INPUT,
           colx(3, 1), 74.f, "TRANSFORM", cellw(3));
    cvKnob(panel, ZPlane::RESO_PARAM,      ZPlane::RESO_CV_PARAM,      ZPlane::RESO_INPUT,
           colx(3, 2), 74.f, "RESO", cellw(3));

    panel.decor.push_back(section(20.f, 172.f, W - 40.f, 112.f, "VOICE"));
    cvKnob(panel, ZPlane::DRIVE_PARAM,  ZPlane::DRIVE_CV_PARAM,  ZPlane::DRIVE_INPUT,
           colx(4, 0), 202.f, "DRIVE", cellw(4));
    cvKnob(panel, ZPlane::KEYTRK_PARAM, ZPlane::KEYTRK_CV_PARAM, ZPlane::KEYTRK_INPUT,
           colx(4, 1), 202.f, "KEY TRK", cellw(4));
    cvKnob(panel, ZPlane::MIX_PARAM,    ZPlane::MIX_CV_PARAM,    ZPlane::MIX_INPUT,
           colx(4, 2), 202.f, "MIX", cellw(4));
    cvKnob(panel, ZPlane::LEVEL_PARAM,  ZPlane::LEVEL_CV_PARAM,  ZPlane::LEVEL_INPUT,
           colx(4, 3), 202.f, "LEVEL", cellw(4));

    // PRESET is a discrete choice, so it gets a readout box rather than a knob:
    // the name is the only useful feedback, and a number would not be.
    rackx::PanelElement psel = el(ZPlane::PRESET_PARAM, W * 0.5f, 292.f, 11.f,
                                  S::SegmentDisplay, "PRESET");
    psel.width = 132.f; psel.height = 22.f;
    panel.params.push_back(psel);
    panel.inputs.push_back(el(ZPlane::PRESET_INPUT, W - 46.f, 292.f, 9.f, S::Knob, "CV"));

    panel.decor.push_back(section(20.f, 316.f, W - 40.f, 62.f, "I / O"));
    const float jx = 52.f, jd = (W - 130.f) / 2.f;
    panel.inputs.push_back(el(ZPlane::AUDIO_INPUT, jx + jd * 0.f, 348.f, 9.f, S::Knob, "IN"));
    panel.inputs.push_back(el(ZPlane::VOCT_INPUT,  jx + jd * 1.f, 348.f, 9.f, S::Knob, "V/OCT"));
    panel.outputs.push_back(el(ZPlane::MAIN_OUTPUT, jx + jd * 2.f, 348.f, 9.f, S::Knob, "OUT"));
    panel.lights.push_back(el(ZPlane::RESO_LIGHT,  W - 52.f, 348.f, 5.f, S::Lamp, "RES"));
    panel.lights.push_back(el(ZPlane::DRIVE_LIGHT, W - 30.f, 348.f, 5.f, S::Lamp, "DRV"));
    return panel;
}

} // namespace

namespace rackx {
void registerZPlaneModule() {
    addType("ZPLANE", "Z-Plane Filter", "Filter", Role::Normal,
            [] { return std::make_unique<ZPlane>(); }, zplanePanel());
}
} // namespace rackx
