//----------------------------------------------------------------------------
//  PatchKnob — compile a Csound document WITHOUT running it.
//
//  The oracle for the in-DAW assistant's write/compile/fix loop: the model's
//  code is compiled by the REAL Csound compiler on a throwaway instance and
//  its diagnostics are captured verbatim, so the loop iterates against what
//  Csound actually says rather than against a heuristic.
//
//  Uses its own CSOUND instance (like CsoundNode::rebuild does), so it never
//  touches a running patch node and is safe to call from a worker thread.
//  It compiles only -- csoundStart() is never called, no device is opened, no
//  audio is produced.
//----------------------------------------------------------------------------
#pragma once
#include <string>

namespace PatchKnob { namespace engine {

struct CsoundDryCompileResult {
    bool        ok = false;      //!< the document compiled
    bool        available = false; //!< false when Csound is not in this build
    std::string diagnostics;     //!< compiler output, verbatim (may be empty)
};

//! Compile `csd` (a whole <CsoundSynthesizer> document) and throw the result
//! away. `sampleRate` only matters for opcodes that validate against it.
CsoundDryCompileResult csoundDryCompile(const std::string& csd,
                                        double sampleRate = 48000.0);

}} // namespace PatchKnob::engine
