//----------------------------------------------------------------------------
//  src/engine/cdp/processes/cdp_submix.cpp
//
//  Ported from the CDP SUBMIX family (vendor/cdp8/dev/submix), which is
//      Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
//      LGPL 2.1 -- see vendor/cdp8/LICENSE
//
//  Exactly which files, and which function in each:
//      mixmerge.c   mixtwo / do_mix2 / do_mix2_stagger  -> submix.merge
//                   mixmany                             -> submix.mergemany
//                   mix_cross / cross / read_cos        -> submix.crossfade
//                   mix_interl / copy_to_channel        -> submix.interleave
//      mixprepro.c  mixtwo_preprocess    -- the gain law skew/(skew+1)
//                   mixcross_preprocess  -- the crossfade layout and its
//                                           begin/end consistency rules
//                   gen_mcr_table        -- the cosine contour
//      setupmix.c   d_assign_scaling, case MONO_TO_STEREO -> submix.pan
//                   (the level/pan law every CDP mixfile line is rendered with)
//
//  THIS IS THE MULTI-SOURCE FAMILY.  Everything here except submix.pan consumes
//  two or more clips, which is what the offline editor's extra input slots are
//  for.  Slot order is meaningful and is documented per process below.
//
//  What was welded on and has been dropped: argv parsing, the `dataptr dz`
//  parameter blocks, sndseek/fgetfbufEx/write_samps against real soundfiles, the
//  two-buffer ping-pong and its "sector arithmetic" (CDP rounds stagger and skip
//  to a 256-sample disk sector -- over contiguous buffers there is no sector),
//  the mono->stereo temp-file conversion mixtwo does when the two inputs have
//  different channel counts, and the breakpoint-file sweeping of the mix
//  parameters.  The arithmetic that decides a sample's value is untouched.
//
//  ---- SOURCE-MISMATCH POLICY, applied identically by every process here ----
//
//  SAMPLE RATE  Sources MUST agree.  A mismatch is REFUSED with a message
//               naming both rates.  Nothing here resamples: silently resampling
//               a source would change its pitch and duration behind the user's
//               back, and CDP itself refuses (mixtwo_sndprops_consistency:
//               "Different sample-rates in input files: can't proceed").
//  LENGTH       The shorter source is PADDED WITH SILENCE, never indexed past
//               its end.  Output length = the longest contributing extent, so
//               with no offset it is simply the longest source.  The one
//               exception is submix.crossfade, whose length is set by the fade
//               layout -- documented on that process, because CDP's crossfade
//               deliberately ends on source 2.
//  CHANNELS     Output takes the LARGEST channel count.  A source with fewer
//               channels is read with its channel index wrapped (c % chans), so
//               mono into a stereo mix lands in both sides, which is what CDP's
//               mixtwo does by converting the mono file to stereo first.
//               submix.interleave is the exception -- there each source
//               contributes exactly one channel, by definition.
//  LEVEL        No process here normalises unless asked.  merge's two gains sum
//               to 1 by construction (CDP's law) and crossfade is a convex
//               blend, so neither can exceed the louder input.  mergemany is a
//               RAW SUM and can exceed full scale; its Normalise switch does
//               what CDP's mixmany always did, and is off by default so that a
//               mix node does not silently re-gain a chain.
//  FINITENESS   Every output is swept for NaN/Inf before it is returned.
//
//  ---- ON STREAMING ----
//  A sample-aligned mix genuinely is sample-by-sample, but Stream::process is
//  handed ONE buffer to transform in place: it cannot express "read slot 0 and
//  slot 1".  So the multi-source processes declare streamable = false and the
//  host renders them offline.  submix.pan is single-source and a pure gain, so
//  it DOES provide makeStream and becomes a zero-latency rack module.
//----------------------------------------------------------------------------
#include "../cdp_process.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace PatchKnob { namespace cdp {

std::vector<Process>& mutable_registry();

namespace {

constexpr double kPi = 3.14159265358979323846;

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

//! Parameter `i`, or `d` when the host has not supplied that many.  A
//! non-finite value falls back to the default too: several of these become a
//! buffer length, and a NaN must not reach an allocation.
inline double at(const std::vector<double>& p, size_t i, double d) {
    return (i < p.size() && std::isfinite(p[i])) ? p[i] : d;
}

//! Sample `f` of channel `c` of one source: SILENCE outside the source, and the
//! channel index wrapped so a narrower source feeds a wider mix.  Every read of
//! a source in this file goes through here, which is what makes indexing past
//! the end of the shorter buffer structurally impossible.
inline float tap(const Buffer& b, int c, int64_t f) {
    const int nc = b.channels();
    if (nc <= 0 || f < 0 || f >= b.frames()) return 0.f;
    return b.ch[(size_t)(c % nc)][(size_t)f];
}

int64_t longest(const std::vector<Buffer>& in) {
    int64_t n = 0;
    for (const Buffer& b : in) n = std::max(n, b.frames());
    return n;
}
int widest(const std::vector<Buffer>& in) {
    int n = 1;
    for (const Buffer& b : in) n = std::max(n, b.channels());
    return n;
}

//! Shared entry check: enough sources, none empty, and one sample rate between
//! them.  Refusing here is the whole point -- a mix of two rates is garbage.
bool accept(const std::vector<Buffer>& in, int need, std::string& err) {
    if ((int)in.size() < need) {
        err = "needs " + std::to_string(need) + " sources, got " +
              std::to_string((int)in.size());
        return false;
    }
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i].empty()) {
            err = "source " + std::to_string((int)i + 1) + " is empty";
            return false;
        }
        if (in[i].sampleRate != in[0].sampleRate) {
            err = "sources have different sample rates (source 1 is " +
                  std::to_string(in[0].sampleRate) + " Hz, source " +
                  std::to_string((int)i + 1) + " is " +
                  std::to_string(in[i].sampleRate) +
                  " Hz) -- resample them to match, this will not do it for you";
            return false;
        }
    }
    return true;
}

//! Nothing leaves this file non-finite.
void sanitise(Buffer& b) {
    for (std::vector<float>& c : b.ch)
        for (float& v : c) if (!std::isfinite(v)) v = 0.f;
}

//----------------------------------------------------------------------------
//  submix.merge -- CDP MIXTWO ("submix merge")
//
//  SLOT 0 is the first sound, SLOT 1 the second.  Slot 1 enters `offset` into
//  slot 0 and is read from `skip` into itself.
//
//  CDP: mixtwo_preprocess sets   gain1 = skew/(skew+1),  gain2 = 1 - gain1
//  and do_mix2 then writes       out = in1*gain1 + in2*gain2 .
//  The gains always sum to 1, so a merge cannot be louder than its loudest
//  input; skew = 1 gives the equal 0.5/0.5 mix, which is CDP's default.
//
//  DELIBERATE DEVIATION -- offset and skip are FRACTIONS, not seconds.  CDP
//  takes -sSTAGGER and -jSKIP in seconds and REFUSES outright when skip runs
//  past the end of file 2 ("SKIP INTO 2ND FILE exceeds length of that file").
//  A node created with a default in seconds would refuse on any short clip, so
//  both are expressed relative to the source they measure, which is meaningful
//  at any duration and cannot be out of range.  Skip is additionally clamped to
//  one frame short of the end rather than refused.
//----------------------------------------------------------------------------
bool merge_run(const std::vector<Buffer>& in, const std::vector<double>& p,
               Buffer& out, std::string& err, const Progress& prog) {
    if (!accept(in, 2, err)) return false;
    const Buffer& A = in[0];
    const Buffer& B = in[1];

    const double skew = std::max(1e-4, at(p, 0, 1.0));
    const double g1 = skew / (skew + 1.0);          // mixtwo_preprocess
    const double g2 = 1.0 - g1;

    const int64_t offset =
        (int64_t)std::llround(clamp01(at(p, 1, 0.0)) * (double)A.frames());
    int64_t skip =
        (int64_t)std::llround(clamp01(at(p, 2, 0.0)) * (double)B.frames());
    skip = std::min(skip, B.frames() - 1);          // never skip the whole file

    const int64_t len = std::max(A.frames(), offset + (B.frames() - skip));
    const int chans = std::max(A.channels(), B.channels());

    out.sampleRate = A.sampleRate;
    out.resize(chans, len);
    for (int c = 0; c < chans; ++c) {
        std::vector<float>& o = out.ch[(size_t)c];
        for (int64_t i = 0; i < len; ++i)
            o[(size_t)i] = (float)(g1 * (double)tap(A, c, i) +
                                   g2 * (double)tap(B, c, i - offset + skip));
        if (prog && !prog((double)(c + 1) / (double)chans)) {
            err = "cancelled"; return false;
        }
    }
    sanitise(out);
    return true;
}

//----------------------------------------------------------------------------
//  submix.mergemany -- CDP MIXMANY ("submix mergemany")
//
//  SLOTS 0..N are summed, all starting together at time zero.  Slot order does
//  not affect the result; it is a plain sum.
//
//  CDP's mixmany makes two passes: the first accumulates the sum to find its
//  peak, the second writes sum * (F_MAXSAMP/peak) -- i.e. it ALWAYS normalises
//  to full scale, because it was writing a fixed-point soundfile that would
//  otherwise wrap.  We keep float headroom, so that pass is a switch, off by
//  default: a mix node inside a chain must not silently re-gain the signal.
//  Turn Normalise on for CDP's exact output.
//----------------------------------------------------------------------------
bool mergemany_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                   Buffer& out, std::string& err, const Progress& prog) {
    if (!accept(in, 2, err)) return false;
    const double gain = std::max(0.0, at(p, 0, 1.0));
    const bool normalise = at(p, 1, 0.0) >= 0.5;

    const int64_t len = longest(in);
    const int chans = widest(in);
    out.sampleRate = in[0].sampleRate;
    out.resize(chans, len);

    double peak = 0.0;
    for (int c = 0; c < chans; ++c) {
        std::vector<float>& o = out.ch[(size_t)c];
        for (int64_t i = 0; i < len; ++i) {
            double s = 0.0;
            for (const Buffer& b : in) s += (double)tap(b, c, i);
            s *= gain;
            if (!std::isfinite(s)) s = 0.0;
            peak = std::max(peak, std::fabs(s));
            o[(size_t)i] = (float)s;
        }
        if (prog && !prog((double)(c + 1) / (double)chans)) {
            err = "cancelled"; return false;
        }
    }
    if (normalise && peak > 1e-12) {                // CDP: F_MAXSAMP / maxdsamp
        const float k = (float)(1.0 / peak);
        for (std::vector<float>& c : out.ch)
            for (float& v : c) v *= k;
    }
    sanitise(out);
    return true;
}

//----------------------------------------------------------------------------
//  submix.crossfade -- CDP MIXCROSS ("submix crossfade")
//
//  SLOT 0 is faded OUT, SLOT 1 is faded IN.  Slot 1 enters `offset` into slot 0
//  (CDP's -sSTAGGER); the fade runs between `fade start` and `fade end`, both
//  measured across the region where the two sources overlap.
//
//  CDP's cross() is
//      x   = crosindex * crosfact          (0 at BEGIN, 1 at END)
//      out = in1*(1-x) + in2*x
//  with three contours: MCR_LINEAR uses x directly, MCR_COSIN maps x through
//  gen_mcr_table's raised cosine, and MCR_SKEWED raises x to POWFAC first.
//  gen_mcr_table builds 512 samples of  1 - (cos(pi*x)+1)/2  and read_cos()
//  linearly interpolates between them; the closed form is evaluated here
//  instead, which is the same curve without the table's interpolation error.
//
//  LENGTH -- the documented exception to the "longest source wins" rule.  CDP's
//  mix_cross fades TOWARDS file 2 and then copies the remainder of file 2 out;
//  whatever is left of file 1 after the fade is discarded, since its gain is
//  zero from there on.  So the output runs to the end of source 1's tail only
//  while it is still audible:  length = offset + frames(slot 1).
//
//  DELIBERATE DEVIATION -- offset/start/end are FRACTIONS, not seconds.  CDP
//  refuses when BEGIN lands after the end of file 2 or when END - BEGIN <= 0
//  ("Crossfade length is zero or negative: Impossible").  Fractions of the
//  overlap cannot land outside it at any clip length, and the degenerate
//  start == end case is clamped to a one-frame hard cut instead of refused.
//----------------------------------------------------------------------------
bool crossfade_run(const std::vector<Buffer>& in, const std::vector<double>& p,
                   Buffer& out, std::string& err, const Progress& prog) {
    if (!accept(in, 2, err)) return false;
    const Buffer& A = in[0];
    const Buffer& B = in[1];

    // Where source 2 enters, and the span over which both are present.
    int64_t offset =
        (int64_t)std::llround(clamp01(at(p, 0, 0.0)) * (double)A.frames());
    offset = std::min(offset, A.frames() - 1);      // guarantee some overlap
    const int64_t overlapEnd = std::min(A.frames(), offset + B.frames());
    const int64_t overlap = std::max<int64_t>(1, overlapEnd - offset);

    int64_t fadeA = offset + (int64_t)std::llround(clamp01(at(p, 1, 0.0)) *
                                                   (double)overlap);
    int64_t fadeB = offset + (int64_t)std::llround(clamp01(at(p, 2, 1.0)) *
                                                   (double)overlap);
    if (fadeB <= fadeA) fadeB = fadeA + 1;          // hard cut, never zero-width
    const double span = (double)(fadeB - fadeA);

    const bool cosine = at(p, 3, 1.0) >= 0.5;
    const double powfac = std::max(0.125, std::min(8.0, at(p, 4, 1.0)));

    const int64_t len = std::max<int64_t>(1, offset + B.frames());
    const int chans = std::max(A.channels(), B.channels());
    out.sampleRate = A.sampleRate;
    out.resize(chans, len);

    for (int c = 0; c < chans; ++c) {
        std::vector<float>& o = out.ch[(size_t)c];
        for (int64_t i = 0; i < len; ++i) {
            double x;
            if (i <= fadeA)      x = 0.0;
            else if (i >= fadeB) x = 1.0;
            else {
                x = (double)(i - fadeA) / span;
                if (cosine) {
                    if (powfac != 1.0) x = std::pow(x, powfac);   // MCR_SKEWED
                    x = (1.0 - std::cos(kPi * x)) * 0.5;          // gen_mcr_table
                    x = clamp01(x);
                }
            }
            const double a = (double)tap(A, c, i);
            const double b = (double)tap(B, c, i - offset);
            o[(size_t)i] = (float)(a * (1.0 - x) + b * x);
        }
        if (prog && !prog((double)(c + 1) / (double)chans)) {
            err = "cancelled"; return false;
        }
    }
    sanitise(out);
    return true;
}

//----------------------------------------------------------------------------
//  submix.interleave -- CDP MIXINTERL ("submix interleave")
//
//  SLOT n becomes CHANNEL n of the output: slot 0 is the left of a stereo pair,
//  or channel 1 of a quad, and so on -- CDP's copy_to_channel writes source n
//  at stride infilecnt starting at offset n, which is exactly this.
//
//  CDP requires mono inputs; here a multichannel source contributes its FIRST
//  channel only, so the promise "slot n is channel n" holds whatever is
//  dragged in.  Output length is the longest source, the shorter ones running
//  out into silence (CDP: samps_left[n] simply stops being read).
//
//  This is the one process whose output channel count is NOT the widest input:
//  it is the number of connected sources, by definition.
//----------------------------------------------------------------------------
bool interleave_run(const std::vector<Buffer>& in, const std::vector<double>&,
                    Buffer& out, std::string& err, const Progress& prog) {
    if (!accept(in, 2, err)) return false;
    const int chans = (int)in.size();
    const int64_t len = longest(in);
    out.sampleRate = in[0].sampleRate;
    out.resize(chans, len);
    for (int c = 0; c < chans; ++c) {
        std::vector<float>& o = out.ch[(size_t)c];
        for (int64_t i = 0; i < len; ++i) o[(size_t)i] = tap(in[(size_t)c], 0, i);
        if (prog && !prog((double)(c + 1) / (double)chans)) {
            err = "cancelled"; return false;
        }
    }
    sanitise(out);
    return true;
}

//----------------------------------------------------------------------------
//  submix.pan -- the level/pan law of a CDP mixfile line
//
//  Single source, so no slot ordering to worry about.  setupmix.c's
//  d_assign_scaling, case MONO_TO_STEREO, is the whole law:
//
//      pan < -1 : L = level / -pan        R = 0            (hard left, quieter)
//      pan < 0  : L = level               R = level*(1+pan)
//      pan <= 1 : L = level*(1-pan)       R = level
//      pan > 1  : L = 0                   R = level / pan  (hard right, quieter)
//
//  The leading side stays at full level and the trailing side is faded out,
//  and beyond +-1 CDP keeps panning "past" the speaker by attenuating -- which
//  is why MINPAN/MAXPAN are +-32767 and not +-1.
//
//  Two gains and no state, so this is the family's zero-latency module: it
//  provides makeStream and the rack runs it sample-by-sample.  Offline a MONO
//  source is spread to stereo (CDP converts mono mixfile entries to stereo
//  before applying the law); live, the stream cannot change the host's channel
//  count, so it applies L to the even channels and R to the odd ones in place.
//----------------------------------------------------------------------------
struct PanGains {
    double l = 1.0, r = 1.0;
    void design(double level, double pan) {
        level = std::max(0.0, level);
        if (pan < -1.0)     { l = level / -pan;        r = 0.0; }
        else if (pan < 0.0) { l = level;               r = level * (1.0 + pan); }
        else if (pan <= 1.0){ l = level * (1.0 - pan); r = level; }
        else                { l = 0.0;                 r = level / pan; }
        if (!std::isfinite(l)) l = 0.0;
        if (!std::isfinite(r)) r = 0.0;
    }
};

PanGains pan_from(const std::vector<double>& p) {
    PanGains g;
    g.design(at(p, 0, 1.0), at(p, 1, 0.0));
    return g;
}

struct PanStream final : Stream {
    PanGains g;
    void process(float* const* ch, int channels, int n) override {
        for (int c = 0; c < channels; ++c) {
            const float k = (float)((c % 2) == 0 ? g.l : g.r);
            float* x = ch[c];
            for (int i = 0; i < n; ++i) x[i] *= k;
        }
    }
    void reset() override {}                        // stateless
};

bool pan_run(const std::vector<Buffer>& in, const std::vector<double>& p,
             Buffer& out, std::string& err, const Progress& prog) {
    if (!accept(in, 1, err)) return false;
    const Buffer& A = in[0];
    const PanGains g = pan_from(p);
    const int chans = std::max(2, A.channels());    // mono is spread to stereo
    out.sampleRate = A.sampleRate;
    out.resize(chans, A.frames());
    for (int c = 0; c < chans; ++c) {
        const double k = ((c % 2) == 0) ? g.l : g.r;
        std::vector<float>& o = out.ch[(size_t)c];
        for (int64_t i = 0; i < A.frames(); ++i)
            o[(size_t)i] = (float)(k * (double)tap(A, c, i));
        if (prog && !prog((double)(c + 1) / (double)chans)) {
            err = "cancelled"; return false;
        }
    }
    sanitise(out);
    return true;
}

//! Multi-source processes cannot stream (Stream carries one buffer), so they
//! all report the same honest zero inherent delay for the offline path.
int64_t no_latency(int, const std::vector<double>&) { return 0; }

} // namespace

void register_submix_processes() {
    std::vector<Process>& r = mutable_registry();
    {
        Process p;
        p.slug  = "submix.merge";
        p.name  = "Merge Two";
        p.group = "Submix";
        p.help  = "Mix two sources with CDP's balance law. Slot 0 is the first "
                  "sound, slot 1 the second; slot 1 can enter late and start "
                  "part way in. The two gains always sum to 1.";
        p.minInputs = 2; p.maxInputs = 2;
        p.streamable = false;
        p.latencyFrames = &no_latency;
        p.params = {
            { "Balance", "", 0.01, 100.0, 1.0, false,
              "Level of slot 0 relative to slot 1; 1 is an equal mix." },
            { "Offset", "of slot 0", 0.0, 1.0, 0.0, false,
              "How far into slot 0 that slot 1 enters, as a fraction." },
            { "Skip", "of slot 1", 0.0, 1.0, 0.0, false,
              "How far into slot 1 to start reading it, as a fraction." },
        };
        p.run = &merge_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "submix.mergemany";
        p.name  = "Merge Many";
        p.group = "Submix";
        p.help  = "Sum any number of sources, all starting together. Shorter "
                  "sources run out into silence; slot order does not matter.";
        p.minInputs = 2; p.maxInputs = 8;
        p.streamable = false;
        p.latencyFrames = &no_latency;
        p.params = {
            { "Gain", "", 0.0, 4.0, 1.0, false, "Applied to the summed mix." },
            { "Normalise", "", 0, 1, 0, true,
              "Scale the finished mix so its peak is full scale, as CDP's "
              "mergemany always did." },
        };
        p.run = &mergemany_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "submix.crossfade";
        p.name  = "Cross-fade";
        p.group = "Submix";
        p.help  = "Fade from slot 0 to slot 1. Positions are fractions of the "
                  "region where the two overlap, so they are valid at any clip "
                  "length. Output ends when slot 1 ends.";
        p.minInputs = 2; p.maxInputs = 2;
        p.streamable = false;      // the layout is set by whole-source lengths
        p.latencyFrames = &no_latency;
        p.params = {
            { "Offset", "of slot 0", 0.0, 1.0, 0.0, false,
              "How far into slot 0 that slot 1 enters, as a fraction." },
            { "Fade start", "of overlap", 0.0, 1.0, 0.0, false,
              "Where the fade begins within the overlapping region." },
            { "Fade end", "of overlap", 0.0, 1.0, 1.0, false,
              "Where the fade ends within the overlapping region." },
            { "Cosine", "", 0, 1, 1, true,
              "Raised-cosine contour (CDP mode 2); off gives a linear fade." },
            { "Skew", "", 0.125, 8.0, 1.0, false,
              "Bends the cosine contour: below 1 starts fast then slows, "
              "above 1 starts slowly then speeds up." },
        };
        p.run = &crossfade_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "submix.interleave";
        p.name  = "Interleave";
        p.group = "Submix";
        p.help  = "Build a multichannel sound from several sources: slot 0 "
                  "becomes channel 1, slot 1 channel 2, and so on. Only the "
                  "first channel of each source is used.";
        p.minInputs = 2; p.maxInputs = 8;
        p.streamable = false;
        p.latencyFrames = &no_latency;
        p.params = {};
        p.run = &interleave_run;
        r.push_back(p);
    }
    {
        Process p;
        p.slug  = "submix.pan";
        p.name  = "Mix Pan";
        p.group = "Submix";
        p.help  = "Level and stereo position, using the pan law CDP applies to "
                  "every line of a mixfile. Past hard left or right it keeps "
                  "going by attenuating.";
        p.minInputs = 1; p.maxInputs = 1;
        p.streamable = true;
        // Two multiplies and no state: nothing to look ahead for.
        p.latencyFrames = [](int, const std::vector<double>&) -> int64_t { return 0; };
        p.params = {
            { "Level", "", 0.0, 4.0, 1.0, false, "Gain, 1 is unity." },
            { "Pan", "", -4.0, 4.0, 0.0, false,
              "-1 hard left, 0 centre, 1 hard right; beyond that, further out "
              "and quieter." },
        };
        p.makeStream = [](int, const std::vector<double>& pr) -> std::unique_ptr<Stream> {
            auto s = std::unique_ptr<PanStream>(new PanStream());
            s->g = pan_from(pr);
            return s;
        };
        p.run = &pan_run;
        r.push_back(p);
    }
}

} } // namespace PatchKnob::cdp
