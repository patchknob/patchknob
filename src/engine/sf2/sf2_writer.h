//----------------------------------------------------------------------------
//  src/engine/sf2/sf2_writer.h -- write a PatchKnob sampler patch as SoundFont 2.
//
//  TWO AUDIENCES, ONE FILE.
//
//  1. Any other soundfont player.  What we emit is a strictly conformant SF2
//     2.04 file: RIFF/sfbk with INFO, sdta (16-bit samples, optional sm24 for
//     the low bytes of 24-bit) and pdta (phdr/pbag/pmod/pgen/inst/ibag/imod/
//     igen/shdr), terminal records present, samples padded with the 46 zero
//     frames the spec demands after each one.  Zones, key and velocity ranges,
//     root keys, loops, tuning, pan, attenuation, filter cutoff/Q, both
//     envelopes and exclusive class all become real generators, so the patch
//     plays correctly in FluidSynth, Polyphone or a hardware sampler.
//
//  2. Us, later.  The things that make our sampler ours have NO generator to
//     map onto -- the native FX tail (gain/pan/width/drive/LP/HP/bitcrush/
//     downsample/noise), tracker note-columns and their per-column voice
//     allocation, and our envelopes, which are dense piecewise-linear curves
//     while SF2 has only six-stage DAHDSR with fixed shapes.  Exporting those
//     through generators alone is lossy: the FX vanish and the curves are
//     FITTED to DAHDSR, not preserved.
//
//     So we also write a private "PKST" chunk carrying that state verbatim.
//     The spec requires a reader to skip chunks it does not recognise, so this
//     costs other players nothing -- and reading our own file back restores the
//     patch exactly, FX chain and curve shapes included.
//
//  Set `standardOnly` when the file is purely for interchange and you want no
//  private data in it at all.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_SF2_WRITER_H
#define PATCHKNOB_ENGINE_SF2_WRITER_H

#include "sf2_reader.h"

#include <string>
#include <vector>

namespace PatchKnob { namespace engine { namespace sf2 {

struct WriteOptions {
    std::string bankName   = "PatchKnob Export";
    std::string author;
    std::string comment;
    //! Emit the standard chunks only -- no PKST.  The file then loses the FX
    //! chain, column configuration and exact envelope curves, which is the
    //! right trade when the file is going to someone else's sampler.
    bool standardOnly = false;
    //! 24-bit sample data (adds the sm24 chunk).  Conformant readers that
    //! predate SF2.04 ignore sm24 and play the 16-bit data on its own.
    bool write24Bit = false;
};

//! Opaque blob of PatchKnob-only patch state (FX chain, per-column setup, exact
//! envelope curves).  The caller serialises it; the writer only stores it.
struct PrivateState { std::vector<uint8_t> bytes; };

//! Write `font` to `path`.  `priv` may be empty.  Returns false + `error` on
//! failure, and never leaves a partial file behind (writes to a temporary and
//! renames), so a failed export cannot destroy an existing soundfont.
bool write(const std::string& path, const SoundFont& font,
           const WriteOptions& opts, const PrivateState& priv,
           std::string& error);

//! Read back a PKST chunk written by us.  False when the file has none, which
//! is the normal case for a third-party soundfont -- not an error.
bool readPrivateState(const std::string& path, PrivateState& out);

//! Fit a dense piecewise-linear envelope (the editor's representation, on the
//! 0..65535 axes) to SF2's six DAHDSR stages.  Exposed because the LOSS belongs
//! in the open where a caller can warn about it -- the UI tells the user their
//! curve will be approximated before it writes the file, rather than after.
struct DahdsrFit {
    float delay = 0.f, attack = 0.f, hold = 0.f, decay = 0.f;
    float sustain = 1.f, release = 0.f;
    float maxErrorDb = 0.f;   //!< worst deviation of the fit from the original
};
DahdsrFit fitEnvelopeToDahdsr(const unsigned short* xs, const unsigned short* ys,
                              const int* flags, int count, float secondsPerUnit);

}}} // namespace PatchKnob::engine::sf2

#endif // PATCHKNOB_ENGINE_SF2_WRITER_H
