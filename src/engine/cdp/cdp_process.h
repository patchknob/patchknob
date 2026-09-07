//----------------------------------------------------------------------------
//  src/engine/cdp/cdp_process.h
//
//  Composer's Desktop Project as a callable library.
//
//  CDP is 244 command-line programs: each transformation lives inside a main()
//  that parses argv, opens soundfiles through CDP's sfsys layer, runs a loop,
//  and writes a file.  There is no DSP entry point to bind to.  So the
//  algorithms are lifted out of those main()s into functions over buffers, and
//  registered here.  Everything the host needs -- the process list, each one's
//  parameters, how many inputs it takes, and whether it can stream -- is data,
//  so the CDP window and the rack modules are both driven from one registry.
//
//  CDP8 is LGPL 2.1 (vendor/cdp8/LICENSE); this project is GPL, which may use
//  it.  Each ported file names the CDP program it came from.
//
//  ON REALTIME: most of the interesting CDP work is spectral (phase vocoder).
//  A PVOC process cannot be zero latency -- it must fill an analysis window
//  before it can emit anything -- so `latencyFrames` reports that honestly and
//  the host compensates.  Processes that ARE sample-by-sample (gain shaping,
//  distortion, waveset work, simple filters) report zero.  Processes whose
//  algorithm needs the whole file up front (anything that scans to normalise,
//  reverses, or re-times) cannot stream at all and say so via `streamable`.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_CDP_PROCESS_H
#define PATCHKNOB_CDP_PROCESS_H

#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace PatchKnob { namespace cdp {

//! One tunable of a process, as the CDP program's usage text describes it.
struct ParamSpec {
    std::string name;          //!< short label for the UI
    std::string unit;          //!< "Hz", "dB", "semitones", "" ...
    double      min = 0.0;
    double      max = 1.0;
    double      def = 0.0;
    bool        integer = false;
    std::string help;          //!< one line, from the CDP documentation
};

//! Multi-channel float audio, deinterleaved, as every process sees it.
struct Buffer {
    int                             sampleRate = 48000;
    std::vector<std::vector<float>> ch;         //!< ch[c][frame]
    int64_t frames() const { return ch.empty() ? 0 : (int64_t)ch[0].size(); }
    int     channels() const { return (int)ch.size(); }
    void    resize(int channels, int64_t frames) {
        ch.assign((size_t)(channels < 1 ? 1 : channels),
                  std::vector<float>((size_t)(frames < 0 ? 0 : frames), 0.f));
    }
    bool empty() const { return frames() == 0; }
};

//! Progress/cancel hook for the long offline transforms.  Return false to abort.
using Progress = std::function<bool(double fraction)>;

//! A live, stateful instance of a process for block-by-block use.
//!
//! Whole-buffer `run` cannot be zero latency in a realtime graph -- the host has
//! to accumulate a chunk before it can call it.  A process whose algorithm is
//! genuinely sample-by-sample (a recursive filter, a gain shaper) can instead
//! offer this, and the rack module then runs it with NO added delay at all.
//! Absent == the host falls back to chunked whole-buffer calls and reports that
//! chunk as latency.
struct Stream {
    virtual ~Stream() = default;
    //! Transform `n` frames in place, per channel.  Called from the audio
    //! thread: no allocation, no locks, no file access.
    virtual void process(float* const* ch, int channels, int n) = 0;
    virtual void reset() = 0;
};

//! A registered CDP transformation.
struct Process {
    std::string slug;          //!< stable id, e.g. "blur.blur"
    std::string name;          //!< display name
    std::string group;         //!< CDP family: "Blur", "Morph", "Distort" ...
    std::string help;

    int  minInputs = 1;        //!< how many source clips it consumes
    int  maxInputs = 1;        //!< > 1 == the window accepts dragged clips

    //! True when the algorithm can run block-by-block over a stream, which is
    //! what makes a rack module possible at all.
    bool streamable = false;
    //! Inherent algorithmic delay in frames at the given sample rate (a PVOC
    //! window, a lookahead).  Zero for sample-by-sample processes.  The host
    //! must delay-compensate by this much.
    std::function<int64_t(int sampleRate, const std::vector<double>& params)>
        latencyFrames;

    std::vector<ParamSpec> params;

    //! Optional: build a live instance for block-by-block processing.  Set this
    //! ONLY when the algorithm truly needs no lookahead -- it is what lets the
    //! generated rack module claim zero latency.
    std::function<std::unique_ptr<Stream>(int sampleRate,
                                          const std::vector<double>& params)> makeStream;

    //! Offline: transform `in` (1..N sources) into `out`.  Returns false and
    //! fills `error` on failure.
    std::function<bool(const std::vector<Buffer>& in,
                       const std::vector<double>& params,
                       Buffer& out, std::string& error,
                       const Progress& progress)> run;
};

//! Every process the build knows about.
const std::vector<Process>& registry();
//! Look one up by slug, or nullptr.
const Process*              find(const std::string& slug);
//! Convenience: run a process by slug.
bool run(const std::string& slug, const std::vector<Buffer>& in,
         const std::vector<double>& params, Buffer& out, std::string& error,
         const Progress& progress = {});

} } // namespace PatchKnob::cdp

#endif
