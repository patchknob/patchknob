//----------------------------------------------------------------------------
//  PatchKnob — Consolidate: render several scheduled pieces into ONE clip.
//
//  This lived inside a lambda in sdlui/main.cpp, where no headless harness
//  could link it, so the one part of Consolidate that decides what you HEAR --
//  gap placement, gain, fade curves, loop wrapping -- was the only part with
//  no rendered-sample test.  It is a pure function of its inputs, so it has no
//  business being welded to the shell.
//
//  PT ch.31 p705: the new file "consists of the entire selection, including
//  any blank space", so the output spans [startFrame, endFrame) exactly and
//  anything not covered by a piece is silence.
//----------------------------------------------------------------------------
#pragma once
#include "audio_clip.h"

#include <vector>

namespace PatchKnob { namespace engine {

//! One source piece to fold into the consolidated output.
struct ConsolidatePiece {
    const AudioClip* clip   = nullptr;
    long long        pos    = 0;      //!< timeline frame the piece starts on
    long long        off    = 0;      //!< source frame it starts reading at
    long long        len    = 0;      //!< frames of timeline it covers
    float            gain   = 1.0f;
    bool             muted  = false;  //!< a muted piece contributes silence
    bool             loop   = false;  //!< wrap the source to fill `len`
    //! Fade envelope, carried on a ScheduledClip so the SAME fadeGain() curve
    //! playback uses is the one baked in here. Default = no fades.
    ScheduledClip    fade;
    bool             hasFade = false;
};

//! Mix `pieces` into one clip covering [startFrame, endFrame).
//!
//! Sample-accurate by construction: a piece contributes to output frame
//! `f - startFrame` from source frame `off + (f - pos)`, so a piece's material
//! keeps its exact alignment to the timeline it was consolidated from.
//! Pieces that fall entirely outside the span contribute nothing.
AudioClip consolidateRender(const std::vector<ConsolidatePiece>& pieces,
                            long long startFrame, long long endFrame,
                            double sampleRate);

}} // namespace PatchKnob::engine
