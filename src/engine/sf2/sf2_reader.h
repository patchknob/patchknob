//----------------------------------------------------------------------------
//  src/engine/sf2/sf2_reader.h -- SoundFont 2 / SF3 reader.
//
//  Parses an .sf2 (or Vorbis-compressed .sf3) into the structure the built-in
//  sampler already speaks: presets made of ZONES, each zone a key range, a
//  velocity range, a sample, a loop and a set of envelope/filter parameters.
//
//  WHY WE PARSE IT OURSELVES.  FluidSynth is the better *decoder* -- it is the
//  reference implementation and handles SF3 sample compression -- but its public
//  API exposes only preset identity (bank, program, name).  It deliberately does
//  not publish per-zone generators, because it renders them itself.  Rendering
//  them ourselves is the whole point: only then do PatchKnob's own features
//  reach soundfont material -- tracker note-columns with per-column voice
//  allocation, the shared sample editor and its destructive ops, slot FX, warp.
//  So we read the generator tables directly (SF2 spec 2.04 sections 7 and 8)
//  and hand the sampler zones it can already play and the user can already edit.
//
//  GENERATOR LAYERING (spec section 9.4) is the part everyone gets wrong:
//      * an INSTRUMENT zone's generators are ABSOLUTE values,
//      * a PRESET zone's generators are OFFSETS added on top,
//      * each level has a "global" zone (one with no terminal generator) whose
//        values are the defaults for its siblings,
//      * a few generators (keyRange, velRange, sampleID, instrument) are not
//        additive and must not be summed.
//  resolveZones() implements exactly that, so callers never see raw chunks.
//
//  THREADING.  Message thread only.  Reading an .sf2 is file I/O and allocates;
//  nothing here may be called from the audio thread.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_SF2_READER_H
#define PATCHKNOB_ENGINE_SF2_READER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace PatchKnob { namespace engine { namespace sf2 {

//! SF2 generator operators we actually consume.  (The spec defines 60; the rest
//! are either unused by real soundfonts or meaningless outside FluidSynth's
//! modulator engine, and are ignored rather than silently mis-applied.)
enum Generator {
    GEN_startAddrsOffset      = 0,
    GEN_endAddrsOffset        = 1,
    GEN_startloopAddrsOffset  = 2,
    GEN_endloopAddrsOffset    = 3,
    GEN_startAddrsCoarse      = 4,
    GEN_endAddrsCoarse        = 12,
    GEN_pan                   = 17,
    GEN_delayVolEnv           = 33,
    GEN_attackVolEnv          = 34,
    GEN_holdVolEnv            = 35,
    GEN_decayVolEnv           = 36,
    GEN_sustainVolEnv         = 37,
    GEN_releaseVolEnv         = 38,
    GEN_initialFilterFc       = 8,
    GEN_initialFilterQ        = 9,
    GEN_delayModEnv           = 25,
    GEN_attackModEnv          = 26,
    GEN_holdModEnv            = 27,
    GEN_decayModEnv           = 28,
    GEN_sustainModEnv         = 29,
    GEN_releaseModEnv         = 30,
    GEN_modEnvToPitch         = 7,
    GEN_modEnvToFilterFc      = 11,
    GEN_keyRange              = 43,
    GEN_velRange              = 44,
    GEN_startloopAddrsCoarse  = 45,
    GEN_initialAttenuation    = 48,
    GEN_endloopAddrsCoarse    = 50,
    GEN_coarseTune            = 51,
    GEN_fineTune              = 52,
    GEN_sampleModes           = 54,
    GEN_scaleTuning           = 56,
    GEN_exclusiveClass        = 57,
    GEN_overridingRootKey     = 58,
    GEN_instrument            = 41,
    GEN_sampleID              = 53,
    GEN_COUNT                 = 60
};

//! One sample header (shdr) plus its decoded PCM.
struct Sample {
    std::string name;
    uint32_t    start = 0, end = 0;          //!< frame offsets into the sample pool
    uint32_t    loopStart = 0, loopEnd = 0;
    uint32_t    sampleRate = 44100;
    uint8_t     originalKey = 60;
    int8_t      pitchCorrection = 0;         //!< cents
    uint16_t    sampleLink = 0;              //!< the other half of a stereo pair
    uint16_t    sampleType = 1;              //!< 1 mono, 2 right, 4 left, 8 ROM
    //! Decoded, de-interleaved-per-sample mono PCM in -1..1.  SF2 stores every
    //! sample as mono; a stereo instrument is two samples joined by sampleLink,
    //! which resolveZones() pairs up so the sampler gets one stereo zone.
    std::vector<float> pcm;
    bool isStereoPair() const { return sampleType == 2 || sampleType == 4; }
};

//! A fully resolved zone: preset-level offsets already folded into the
//! instrument-level absolutes, ranges intersected, sample resolved.
struct Zone {
    int  sampleIndex = -1;
    int  loKey = 0,  hiKey = 127;
    int  loVel = 0,  hiVel = 127;
    int  rootKey = 60;               //!< after overridingRootKey
    int  coarseTune = 0;             //!< semitones
    int  fineTune = 0;               //!< cents (pitchCorrection already folded in)
    int  scaleTuning = 100;          //!< cents per key; 100 = normal, 0 = fixed pitch
    int  exclusiveClass = 0;         //!< non-zero: cuts other voices of the class
    float pan = 0.f;                 //!< -1..+1
    float attenuationDb = 0.f;       //!< positive = quieter
    bool  loop = false;
    bool  loopUntilRelease = false;  //!< sampleModes 3
    uint32_t loopStart = 0, loopEnd = 0;   //!< absolute frames within the sample
    uint32_t startOffset = 0, endOffset = 0;

    //! Volume envelope, seconds (sustain is a LEVEL, 0..1).
    float delayVol = 0.f, attackVol = 0.f, holdVol = 0.f, decayVol = 0.f;
    float sustainVol = 1.f, releaseVol = 0.f;
    //! Modulation envelope + its destinations.
    float delayMod = 0.f, attackMod = 0.f, holdMod = 0.f, decayMod = 0.f;
    float sustainMod = 1.f, releaseMod = 0.f;
    float modEnvToPitchCents = 0.f, modEnvToFilterCents = 0.f;
    //! Low-pass filter.  cutoffHz == 0 means "no filter" (the spec's 13500
    //! absolute-cents default is above audibility and is normalised away here).
    float cutoffHz = 0.f, resonanceDb = 0.f;
};

struct Preset {
    std::string      name;
    int              bank = 0, program = 0;
    std::vector<Zone> zones;
};

//! A parsed soundfont.  `samples` is shared by every zone that references it.
struct SoundFont {
    //! Where the sample pool lives IN THE FILE.  Headers-only reads record it
    //! and stop; PCM is then pulled per sample with a seek, so browsing an
    //! 800 MB soundfont costs a few MB and a few milliseconds instead of
    //! reading the whole pool into RAM to look at a preset list.
    std::string          sourcePath;
    uint64_t             smplOffset = 0, smplBytes = 0;
    uint64_t             sm24Offset = 0, sm24Bytes = 0;

    std::string          name, engine, tools, comment;
    int                  versionMajor = 2, versionMinor = 1;
    std::vector<Sample>  samples;
    std::vector<Preset>  presets;   //!< sorted by (bank, program)
    bool empty() const { return presets.empty(); }
};

//! Parse `path`.  Returns false and fills `error` on failure; never throws and
//! never partially publishes -- `out` is untouched unless the whole file parsed.
//!
//! `loadPcm == false` (the DEFAULT) reads headers only (fast: for a browser
//! that lists banks and presets without paying for a 2 GB sample pool).  Zones
//! still resolve; Sample::pcm is simply left empty, and readPresetPcm() /
//! readSamplePcm() pull exactly the audio a caller ends up needing.
//!
//! `loadPcm == true` decodes the ENTIRE pool up front -- an 800 MB bank
//! becomes ~1.6 GB of float PCM.  That is never what an interactive caller
//! wants, which is why headers-only is the default: with eager decode as the
//! default, one forgotten argument stalls the app for seconds and doubles the
//! bank's size in RAM.  Pass true only for offline whole-bank work (e.g. a
//! bank-to-bank converter that really touches every sample).
bool read(const std::string& path, SoundFont& out, std::string& error,
          bool loadPcm = false);

//! Decode ONE sample's PCM after a headers-only read (browser -> audition/import).
bool readSamplePcm(const std::string& path, const SoundFont& font, int sampleIndex,
                   std::vector<float>& out, std::string& error);

//! HEADER-level verdict on whether `sampleIndex` and its sampleLink partner
//! really form a playable stereo pair: partner in range, opposite channel,
//! same frame count (from shdr, so no PCM needs to be decoded to answer).
//! This is the single definition of "sound pair" shared by readPresetPcm()
//! (which must not waste I/O decoding a partner that can never interleave)
//! and the importer (which must interleave exactly the pairs the reader
//! decoded for) -- two copies of this predicate WILL drift.
//! On success `outPartner` is the partner's sample index.
bool samplesFormStereoPair(const SoundFont& font, int sampleIndex, int& outPartner);

//! Decode every sample ONE preset needs, in one pass, into `font.samples[].pcm`
//! (stereo partners included).  This is what "load only the patches you need"
//! costs: a few MB for a patch out of an 800 MB bank.
bool readPresetPcm(const std::string& path, SoundFont& font, int presetIndex,
                   std::string& error);

//! Progress/cancellation hook for a batch decode.  Called with (done, total)
//! after each sample decoded (and once with (0, total) before the first read
//! so a caller learns the total early).  Return false to CANCEL: the decode
//! stops at the next sample boundary and the batch call fails with
//! error == "cancelled".  Called on whatever thread runs the decode.
using PcmProgressFn = std::function<bool(int done, int total)>;

//! Decode an explicit SET of samples (want[i] != 0, one flag per
//! font.samples entry) in one pass -- one open, reads issued in file order.
//! This is readPresetPcm() with the want-set supplied by the caller, for
//! callers that can prove they need less than a whole preset: a capped
//! preview import has no business decoding the 150 samples past its cap.
//! Samples already carrying pcm are left alone.
//! `progress` (optional) reports per-sample completion and can cancel; see
//! PcmProgressFn.  Existing callers compile unchanged.
bool readSamplesPcm(const std::string& path, SoundFont& font,
                    const std::vector<uint8_t>& want, std::string& error,
                    const PcmProgressFn& progress = {});

//! True if the file starts with a RIFF/sfbk signature -- cheap enough to run
//! over a whole directory while browsing.
bool looksLikeSoundFont(const std::string& path);

}}} // namespace PatchKnob::engine::sf2

#endif // PATCHKNOB_ENGINE_SF2_READER_H
