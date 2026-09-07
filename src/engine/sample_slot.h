//----------------------------------------------------------------------------
//  src/engine/sample_slot.h
//
//  ISampleSlot -- the contract between "something that holds ONE audio sample"
//  and the shared sample editor / disk browser UI.
//
//  It lives in the neutral engine layer rather than in the rack factory because
//  BOTH samplers implement it:
//      * the SMPL-1 rack module  (rackx::Sampler1)
//      * the Buzz-hosted Sampler instrument, via a thin adapter over the
//        audio_app_sampler_* C API
//  so one editor widget and one browser widget serve both.
//
//  THREADING.  Every method here is MESSAGE-THREAD only.  Implementations own
//  audio the render thread is reading, so they must take their own lock inside
//  these calls -- the UI never sees a raw sample pointer, which is why the
//  waveform is fetched as PEAKS rather than as a buffer borrow.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_SAMPLE_SLOT_H
#define PATCHKNOB_ENGINE_SAMPLE_SLOT_H

#include <cstdint>
#include <string>
#include <vector>

namespace PatchKnob { namespace engine {

//! Destructive operations a slot may support (see ISampleSlot::sampleOp).
enum SampleOp {
    SOP_NORMALIZE = 0,  //!< scale so the range peaks at `arg` (1.0 == full)
    SOP_REVERSE,        //!< reverse the range in place
    SOP_FADE_IN,        //!< `arg` = curve, -1 log .. 0 linear .. +1 exp
    SOP_FADE_OUT,
    SOP_SILENCE,
    SOP_GAIN,           //!< multiply the range by `arg`
    SOP_DC_REMOVE,      //!< subtract the range's mean
    SOP_TRIM,           //!< discard everything outside the range
    SOP_CROSSFADE_LOOP, //!< `arg` = crossfade length in frames; see below
    SOP_INSERT_SILENCE, //!< insert (to-from) frames of silence at `from`
    SOP_CUT,            //!< copy the range to the clipboard, then close the gap
    SOP_COPY,
    SOP_PASTE_INSERT,   //!< insert the clipboard at `from`
    SOP_PASTE_MIX,      //!< sum the clipboard over the audio at `from`
    SOP_DELETE,         //!< remove the range and close the gap
    SOP_DUPLICATE,      //!< copy the range and insert it directly after itself
    SOP_INVERT,         //!< flip phase
    SOP_SWAP_LR,
    SOP_MONO,           //!< sum to mono (both channels equal)
    SOP_DECLICK,        //!< tiny fades at both edges of the range
    SOP_AUTOTRIM,       //!< drop leading/trailing silence below `arg` (linear)
    SOP_RESAMPLE,       //!< resample the WHOLE file by factor `arg`
    SOP_BITCRUSH,       //!< quantise to `arg` steps
    SOP_COUNT
};

//! Editable positions within a sample, all normalised 0..1 across the file.
enum SampleMarker {
    SM_START = 0,      //!< playback start
    SM_END,            //!< playback end
    SM_LOOP_START,
    SM_LOOP_END,
    SM_COUNT
};

class ISampleSlot {
public:
    virtual ~ISampleSlot() {}

    // ---- content ---------------------------------------------------------
    //! Decode `path` and swap it in.  Returns false and fills `error` on failure.
    virtual bool sampleLoad( const std::string& path, double sampleRate,
                             std::string* error ) = 0;
    virtual void sampleClear() = 0;
    //! Display name of the loaded sample ("" when empty).
    virtual const char* sampleName() const = 0;
    //! FULL source path of the loaded sample ("" when empty or not file-backed).
    //! Kept as metadata only -- the audio itself is embedded (see below), so a
    //! project stays self-contained even if the source file moves or is edited.
    virtual const char* samplePath() const { return ""; }

    // ---- persistence -----------------------------------------------------
    //! Copy the slot's audio out for saving.  Returns false when the slot has
    //! nothing or does not support persistence.
    virtual bool sampleReadAudio( std::vector<float>& outL, std::vector<float>& outR,
                                  double& outRate ) const {
        (void)outL; (void)outR; (void)outRate; return false;
    }
    //! Install audio directly (project load), bypassing the file decoder so an
    //! EDITED or recorded sample restores exactly as it was saved.
    virtual bool sampleSetAudio( const std::vector<float>& inL, const std::vector<float>& inR,
                                 double rate, const std::string& name,
                                 const std::string& path ) {
        (void)inL; (void)inR; (void)rate; (void)name; (void)path; return false;
    }
    virtual int  sampleFrames() const = 0;
    virtual double sampleRate() const { return 48000.0; }

    // ---- waveform --------------------------------------------------------
    //! Fill `buckets` min/max pairs spanning frames [from,to).  The
    //! implementation takes its own lock, so the caller never holds a pointer
    //! into audio the render thread may swap.  Returns the number written
    //! (0 when empty).  `outMin`/`outMax` must have room for `buckets`.
    virtual int samplePeaks( int64_t from, int64_t to,
                             float* outMin, float* outMax, int buckets ) const = 0;

    // ---- markers ---------------------------------------------------------
    virtual float sampleMarker( int marker ) const = 0;
    virtual void  sampleSetMarker( int marker, float value01 ) = 0;
    //! False hides the marker (e.g. loop points while looping is off).
    virtual bool  sampleMarkerActive( int marker ) const { (void)marker; return true; }

    //! Loop on/off.  Playback runs START -> ... -> LOOP END, then repeats the
    //! LOOP START..LOOP END span; so the head of the sample is an attack that
    //! plays once and the loop is the sustain.
    virtual bool sampleLoopEnabled() const { return false; }
    virtual void sampleSetLoopEnabled( bool on ) { (void)on; }

    // ---- loop crossfade (NON-destructive) ---------------------------------
    //  The seamless loop is a PLAYBACK property, not an edit: the player fades
    //  the approach to the loop end into the audio just before the loop start,
    //  so the splice is inaudible while the file on disk is untouched.  The
    //  length is normalised against the whole file, like every other marker,
    //  so the editor can draw it without knowing frame counts.
    virtual bool  sampleXfadeSupported() const { return false; }
    virtual float sampleXfade() const { return 0.f; }
    virtual void  sampleSetXfade( float length01 ) { (void)length01; }

    // ---- slices ----------------------------------------------------------
    //  A slot that chops one file into playable regions (the multisample
    //  instrument) opts in here; a plain one-shot player leaves it off and the
    //  editor simply draws no slice lane.  Positions are normalised 0..1 and
    //  kept sorted by the implementation.
    // ---- destructive editing ---------------------------------------------
    //  The editor never touches audio itself: it names an OPERATION over a
    //  frame range and the slot performs it under its own lock.  That keeps the
    //  widget generic and means the render thread can never observe a partial
    //  edit.  A slot that cannot be edited leaves sampleEditable() false and the
    //  editor greys its toolbar out.
    virtual bool sampleEditable() const { return false; }
    //! `arg` meaning depends on the op (gain factor, fade curve, xfade frames...).
    virtual bool sampleOp( int op, int64_t from, int64_t to, float arg ) {
        (void)op; (void)from; (void)to; (void)arg; return false;
    }
    virtual bool sampleUndo() { return false; }
    virtual bool sampleRedo() { return false; }
    virtual bool sampleCanUndo() const { return false; }
    virtual bool sampleCanRedo() const { return false; }
    //! Peak and RMS of a range (for the level readout).  Returns false if empty.
    virtual bool sampleStats( int64_t from, int64_t to,
                              float* peak, float* rms ) const {
        (void)from; (void)to; (void)peak; (void)rms; return false;
    }
    //! Write the slot's audio to `path` as a WAV.  False if unsupported.
    virtual bool sampleSave( const std::string& path, std::string* error ) {
        (void)path; if ( error ) *error = "not supported"; return false;
    }
    //! True when the clipboard holds audio (enables paste in the UI).
    virtual bool sampleHasClipboard() const { return false; }
    //! Nearest zero crossing to `frame` (dir: -1 back, +1 forward, 0 either).
    //! Used for click-free trims and loop splices.
    virtual int64_t sampleZeroCross( int64_t frame, int dir ) const {
        (void)dir; return frame;
    }

    virtual bool  slicesSupported() const { return false; }
    virtual int   sliceCount() const { return 0; }
    virtual float sliceAt( int index ) const { (void)index; return 0.f; }
    //! Insert a slice; returns its index, or -1 if unsupported/full.
    virtual int   sliceAdd( float value01 ) { (void)value01; return -1; }
    virtual bool  sliceRemove( int index ) { (void)index; return false; }
    virtual void  sliceSet( int index, float value01 ) { (void)index; (void)value01; }
};

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_SAMPLE_SLOT_H
