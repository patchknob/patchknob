//----------------------------------------------------------------------------
//  src/engine/cdp/cdp_pvoc.h
//
//  Phase-vocoder scaffold shared by every SPECTRAL CDP port.
//
//  CDP's spectral programs do not work on samples.  They work on an analysis
//  file produced by its `pvoc` program: a stream of frames, each holding one
//  AMPLITUDE and one FREQUENCY per bin, interleaved -- which is why the CDP
//  sources all walk their data as
//
//      for (vc = 0; vc < dz->wanted; vc += 2) { ... flbufptr[0][AMPP] ... }
//
//  Frame::amp[bin] / Frame::freq[bin] below IS that layout, split into two
//  arrays.  A ported spectral process therefore reads very close to its
//  original: analyse, walk the bins, resynthesise.
//
//  Building this ONCE matters.  Every spectral family (blur, morph, stretch,
//  pitch, spec) needs the same analysis, and six separate implementations would
//  be six separate sets of phase bugs -- and would not agree with each other
//  when chained in the editor.
//
//  LATENCY IS REAL HERE.  A phase vocoder cannot emit anything until it has
//  filled an analysis window, so every process built on this reports
//  `fftSize` frames of delay and is honest that it is not zero-latency.  This
//  is the constraint that makes the spectral families offline-first.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_CDP_PVOC_H
#define PATCHKNOB_CDP_PVOC_H

#include "cdp_process.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace PatchKnob { namespace cdp {

//! One analysis frame: amplitude and TRUE frequency (Hz) per bin.
//!
//! `phase` is ours, not CDP's -- CDP's analysis file carries only the amp/freq
//! pair.  It is the raw analysis phase, kept so synthesis can PHASE-LOCK: a
//! sinusoid does not land in one bin but smeared across a window main lobe, and
//! those bins must keep their RELATIVE phase or they partially cancel on
//! resynthesis.  Integrating each bin independently (the naive vocoder) loses
//! that relation the moment a process alters the frames, and the reconstructed
//! partial then wanders in amplitude -- audible as the classic phase-vocoder
//! warble.  A process that rewrites amp/freq should carry `phase` along with
//! them, or clear it to fall back to independent integration.
struct Frame {
    std::vector<float> amp;
    std::vector<float> freq;
    std::vector<float> phase;
    int bins() const { return (int)amp.size(); }
};

//! Analysis/synthesis settings.  Defaults match what CDP's pvoc uses most.
struct PvocSpec {
    int fftSize = 1024;      //!< power of two
    int overlap = 4;         //!< hop = fftSize / overlap
    int sampleRate = 48000;
    int hop() const { return fftSize / (overlap < 1 ? 1 : overlap); }
    int bins() const { return fftSize / 2 + 1; }
};

//! Analyse one channel into frames of amp/freq pairs.
std::vector<Frame> pvoc_analyse(const std::vector<float>& x, const PvocSpec& s);

//! Resynthesise frames back to samples.  `outFrames` is the length to produce;
//! pass 0 to derive it from the frame count (which is how a time-stretch grows
//! or shrinks its output).
//!
//! `skipSamples` is how much head to discard: analysis pads one window at the
//! front, so the default (-1) drops exactly that and the output lines up with
//! the input.  A process that RESAMPLES THE FRAME SEQUENCE has stretched that
//! padding too and must say how long it now is -- otherwise a 2x stretch starts
//! with a window of stretched silence and everything after it is late.
std::vector<float> pvoc_synthesise(const std::vector<Frame>& frames,
                                   const PvocSpec& s, int64_t outFrames = 0,
                                   int64_t skipSamples = -1);

//! Convenience for the common shape: analyse every channel, let `edit` rewrite
//! the frame list, resynthesise.  `edit` may change the NUMBER of frames (that
//! is how stretching works) -- pass keepLength=false when it does.
bool pvoc_process(const Buffer& in, Buffer& out, const PvocSpec& s,
                  const std::function<void(std::vector<Frame>&)>& edit,
                  bool keepLength = true);

//! One analysis window of delay, in frames -- what every spectral process must
//! report through Process::latencyFrames.
inline int64_t pvoc_latency(const PvocSpec& s) { return s.fftSize; }

} } // namespace PatchKnob::cdp

#endif
