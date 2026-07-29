//----------------------------------------------------------------------------
//  PatchKnob — WAV file loader for AudioClip.
//
//  A small, self-contained RIFF/WAVE parser (no external dependencies). It
//  understands the common uncompressed encodings a DAW needs:
//
//      * PCM       (fmt tag 0x0001) : 16 / 24 / 32-bit signed integer
//      * IEEE float(fmt tag 0x0003) : 32-bit float
//      * WAVE_FORMAT_EXTENSIBLE (0xFFFE) resolved via its sub-format GUID
//      * mono or stereo (>2 channels: the first two are kept)
//
//  On load the samples are converted to float in [-1, 1] and, if the file's
//  sample rate differs from the requested engine rate, resampled with linear
//  interpolation so the resulting AudioClip is at engine rate (ready to drop
//  straight into the AudioClipPlayer schedule). The original file rate is
//  preserved in AudioClip::sourceSampleRate for display.
//
//  All of this runs on the message thread (file I/O + allocation), never the
//  audio thread.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_AUDIOCLIP_WAV_LOADER_H
#define PATCHKNOB_ENGINE_AUDIOCLIP_WAV_LOADER_H

#include <string>

#include "audio_clip.h"

namespace PatchKnob { namespace engine {

//! Load `path` into `out`, resampling to `engineSampleRate` if needed.
//! Returns true on success. On failure `out` is left empty and, if `error` is
//! non-null, a human-readable reason is written to it. Message thread only.
bool loadWav(const std::string& path,
             double engineSampleRate,
             AudioClip& out,
             std::string* error = nullptr);

//! Write a clip out as a 16-bit PCM stereo WAV at its stored sample rate.
//! Handy for round-tripping / debugging and used by the module self-test.
//! Returns true on success. Message thread only.
bool saveWav16(const std::string& path, const AudioClip& clip,
               std::string* error = nullptr);

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_AUDIOCLIP_WAV_LOADER_H
