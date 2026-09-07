//----------------------------------------------------------------------------
//  src/engine/sampler/sampler_declick_test.cpp
//
//  Headless measurement for two properties of the native sampler that are easy
//  to assert and hard to actually have:
//
//  (1) CLICK/POP FREEDOM.  Renders the exact offending case from the bug report
//      -- a repeating 128th-note pattern on ONE tracker note-column (so every
//      note retriggers the column's voice) while "Sample Offset" is automated,
//      so every retrigger starts reading somewhere different in the waveform.
//      A click is, physically, a large single-sample jump, so the metric is
//      direct: for every pair of adjacent output samples take |x[n]-x[n-1]| and
//      report the maximum plus how many exceed a threshold.
//
//      Two numbers keep that metric honest:
//        * `p99.9` -- the 99.9th percentile of the same |diff| distribution.
//          Content slew sets the bulk of the distribution; a click is an
//          OUTLIER.  So maxJump >> p99.9 means clicks, and maxJump ~= p99.9
//          means the largest jump in the render is just the waveform moving.
//          This self-calibrates, which matters when 32 voices sum and the
//          material's own slew rises with them.
//        * peak/rms -- a "fix" that merely silenced the instrument would score a
//          perfect zero on the jump metric, so the level has to be shown to stay
//          comparable for any of it to mean anything.
//
//  (2) PER-TRACKER-COLUMN FX ISOLATION.  Automating an FX on one note column
//      must not perturb another column's voices by so much as one bit.  Proven
//      by bit-exact comparison against a reference render, not by ear and not by
//      an epsilon: see runIsolation() below for why each of the three cases is
//      needed and what a failure in each one means.
//
//  Build (standalone, no CMake needed):
//     g++ -std=c++17 -O2 -I.. sampler_declick_test.cpp samplerinstrument.cpp -o t
//  or configure the sampler lib with -DPATCHKNOB_SAMPLER_TESTS=ON.
//  Run with --post once the declick parameters exist; with no argument it
//  measures whatever engine it was linked against.
//----------------------------------------------------------------------------
#include "sampler_instrument.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace PatchKnob::engine;

// Native parameter ids.  Held as plain numbers so this one source file compiles
// and runs against BOTH the pre-fix engine (24 params) and the post-fix engine
// (28 params) -- the new ids are only written when paramCount() reports them.
enum : uint32_t {
    P_OUTPUT_GAIN = 0, P_PAN = 1, P_WIDTH = 2, P_DRIVE = 3, P_LP = 4, P_HP = 5,
    P_BITDEPTH = 6, P_DOWNSAMPLE = 7, P_TRANSPOSE = 8, P_VELOCITY = 9,
    P_KEY_LOW = 10, P_KEY_HIGH = 11, P_MONO = 12, P_SWAP = 13, P_NOISE = 14,
    P_OFFSET = 15, P_GRANULAR = 16,
    // appended by the declick work
    P_DECLICK = 24, P_ZERO_SNAP = 25, P_OFFSET_MODE = 26, P_SMEAR_GRAIN = 27
};

static const double kSampleRate = 48000.0;
static const int    kBlock      = 128;
// 128th notes at 120 BPM: a whole note is 2 s, so one 128th is 15.625 ms.
static const int    kNotePeriod = (int)(48000.0 * 2.0 / 128.0);   // 750 samples
static const int    kNotePeriod2 = 1000;   // second column, deliberately coprime

static int g_fail = 0;

//----------------------------------------------------------------------------
// Source material: a sustained three-partial tone.  Sustained (not decaying) so
// the amplitude at an arbitrary offset is always substantial, which is exactly
// what makes an un-ramped offset start audible.
//----------------------------------------------------------------------------
static std::vector<float> makeSample(int frames) {
    std::vector<float> pcm((size_t)frames);
    for (int i = 0; i < frames; ++i) {
        const double t = (double)i / kSampleRate;
        pcm[(size_t)i] = (float)(0.50 * std::sin(6.283185307 * 137.0 * t)
                               + 0.30 * std::sin(6.283185307 * 311.0 * t + 0.7)
                               + 0.20 * std::sin(6.283185307 * 523.0 * t + 2.1));
    }
    return pcm;
}
// Largest |x[n]-x[n-1]| the source material itself can produce.
static double contentSlew() {
    const double w = 6.283185307 / kSampleRate;
    return 0.50 * w * 137.0 + 0.30 * w * 311.0 + 0.20 * w * 523.0;
}

struct Metrics {
    double    maxJump = 0.0, p999 = 0.0;
    long long over05 = 0, over10 = 0, over20 = 0;
    double    peak = 0.0, rms = 0.0;
    int       notes = 0;
};

struct Config {
    float declick    = -1.f;   // <0 = leave at the engine default
    float zeroSnap   = -1.f;
    float offsetMode = -1.f;   // 0 clean, 1 akai
    bool  column     = true;   // tag notes to a tracker note-column
    bool  noteOff    = false;
    bool  autoOffset = true;   // sweep the GLOBAL Sample Offset
    // --- per-column isolation knobs ---
    bool  playCol1   = false;  // note-column 1 also plays (note 64)
    bool  playCol0   = true;   // note-column 0 plays (note 60)
    float col1Gain   = -1.f;   // per-column volume for column 1 (<0 = untouched)
    bool  autoCol1   = false;  // automate downsample + offset ON COLUMN 1 only
};

//----------------------------------------------------------------------------
// Render the pattern, measure it, and optionally capture the raw output.
//----------------------------------------------------------------------------
static Metrics run(const Config& cfg, double seconds = 4.0,
                   std::vector<float>* capture = nullptr) {
    IPluginInstance* inst = create_sampler_instrument();
    if (!inst) { std::printf("FATAL: no sampler instrument\n"); std::exit(2); }
    inst->prepare(kSampleRate, kBlock);

    const int sampFrames = (int)kSampleRate;              // 1 second of material
    std::vector<float> pcm = makeSample(sampFrames);
    sampler_load_sample_ex(inst, /*slot*/1, /*level*/0, pcm.data(), sampFrames,
                           /*stereo*/false, /*root*/60, (int)kSampleRate,
                           /*loopStart*/0, /*loopEnd*/sampFrames, /*loop*/true,
                           /*loKey*/0, /*hiKey*/127, /*loVel*/0, /*hiVel*/127,
                           /*noteOffLayer*/false, /*keyToPitch*/true,
                           /*velToVol*/true, /*overlapMode*/0, "test");

    const int pc = inst->paramCount();
    auto setIfPresent = [&](uint32_t id, float v) {
        if (v >= 0.f && (int)id < pc) inst->setParamNormalized(id, v);
    };
    setIfPresent(P_DECLICK,     cfg.declick);
    setIfPresent(P_ZERO_SNAP,   cfg.zeroSnap);
    setIfPresent(P_OFFSET_MODE, cfg.offsetMode);

    // Note 60 belongs to tracker note-column 0, note 64 to column 1.
    if (cfg.column) {
        sampler_set_note_column(inst, 60, 0);
        sampler_set_note_column(inst, 64, 1);
    }
    if (cfg.col1Gain >= 0.f)
        sampler_set_column_param(inst, 1, (int)P_OUTPUT_GAIN, cfg.col1Gain);

    const int total = (int)std::lround(kSampleRate * seconds);
    std::vector<float> bufL((size_t)kBlock), bufR((size_t)kBlock);
    float* outs[2] = { bufL.data(), bufR.data() };
    if (capture) capture->reserve((size_t)total);

    Metrics m;
    double prev = 0.0, sumSq = 0.0;
    std::vector<double> diffs;
    diffs.reserve((size_t)total);

    for (int start = 0; start < total; start += kBlock) {
        const int n = std::min(kBlock, total - start);
        const double t = (double)start / kSampleRate;

        // --- automation: the offset never settles, exactly as reported -------
        ParamChange pcs[1];
        int npc = 0;
        if (cfg.autoOffset) {
            pcs[0].id = P_OFFSET; pcs[0].sampleOffset = 0;
            pcs[0].value = (float)(0.25 + 0.24 * std::sin(6.283185307 * t / 1.7));
            npc = 1;
        }
        // --- FX automation addressed to NOTE-COLUMN 1 ONLY -------------------
        if (cfg.autoCol1) {
            sampler_set_column_param(inst, 1, (int)P_DOWNSAMPLE,
                                     (float)(0.5 + 0.5 * std::sin(6.283185307 * t / 0.31)));
            sampler_set_column_param(inst, 1, (int)P_OFFSET,
                                     (float)(0.5 + 0.4 * std::sin(6.283185307 * t / 0.77)));
        }

        // --- the note pattern ------------------------------------------------
        MidiEvent evs[16];
        int ne = 0;
        for (int k = 0; k < n && ne < 12; ++k) {
            const int abs = start + k;
            if (cfg.playCol0 && abs % kNotePeriod == 0) {
                if (cfg.noteOff && abs > 0) {
                    MidiEvent& off = evs[ne++];
                    off.sampleOffset = k; off.status = 0x80; off.data1 = 60; off.data2 = 0;
                }
                MidiEvent& on = evs[ne++];
                on.sampleOffset = k; on.status = 0x90; on.data1 = 60; on.data2 = 100;
                ++m.notes;
            }
            if (cfg.playCol1 && abs % kNotePeriod2 == 0) {
                MidiEvent& on = evs[ne++];
                on.sampleOffset = k; on.status = 0x90; on.data1 = 64; on.data2 = 100;
            }
        }
        std::stable_sort(evs, evs + ne, [](const MidiEvent& a, const MidiEvent& b) {
            return a.sampleOffset < b.sampleOffset;
        });

        ProcessBlock blk{};
        blk.audioIn = nullptr; blk.audioOut = outs; blk.nframes = n;
        blk.midiIn = evs; blk.numMidiIn = ne;
        blk.paramIn = pcs; blk.numParamIn = npc;
        blk.tempoBpm = 120.0; blk.playPositionSamples = start; blk.isPlaying = true;
        blk.numAudioIn = 0; blk.numAudioOut = 2;
        inst->process(blk);

        for (int k = 0; k < n; ++k) {
            const double x = bufL[(size_t)k];
            if (capture) capture->push_back(bufL[(size_t)k]);
            const double d = std::fabs(x - prev);
            diffs.push_back(d);
            if (d > m.maxJump) m.maxJump = d;
            if (d > 0.05) ++m.over05;
            if (d > 0.10) ++m.over10;
            if (d > 0.20) ++m.over20;
            prev = x;
            const double a = std::fabs(x);
            if (a > m.peak) m.peak = a;
            sumSq += x * x;
        }
    }
    if (!diffs.empty()) {
        m.rms = std::sqrt(sumSq / (double)diffs.size());
        const size_t idx = (size_t)((double)(diffs.size() - 1) * 0.999);
        std::nth_element(diffs.begin(), diffs.begin() + (long)idx, diffs.end());
        m.p999 = diffs[idx];
    }
    inst->release();
    delete inst;
    return m;
}

static void report(const char* label, const Metrics& m) {
    std::printf("  %-33s maxJump=%7.4f p99.9=%6.4f  >0.05=%6lld >0.10=%6lld "
                ">0.20=%6lld   peak=%5.3f rms=%5.3f\n",
                label, m.maxJump, m.p999, m.over05, m.over10, m.over20,
                m.peak, m.rms);
}

//----------------------------------------------------------------------------
// PER-COLUMN FX ISOLATION -- bit-exact, because "close enough" is exactly the
// failure mode being hunted.  The trap is making the PARAMETER per-column while
// leaving the DSP STATE shared: it looks wired up and is still wrong, because
// two columns sharing one decimator phase counter means column 2's notes get
// chopped on column 1's clock even with perfectly correct per-column values.
//----------------------------------------------------------------------------
static bool identical(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}
static long long firstDiff(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return (long long)i;
    return -1;
}
static double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    double d = 0.0;
    for (size_t i = 0; i < n; ++i) d = std::max(d, std::fabs((double)a[i] - (double)b[i]));
    return d;
}

static void expectIdentical(const char* what, const std::vector<float>& ref,
                            const std::vector<float>& got) {
    if (identical(ref, got)) {
        std::printf("  PASS  %s\n         bit-identical over %zu samples\n", what, ref.size());
    } else {
        ++g_fail;
        std::printf("  FAIL  %s\n         diverges at sample %lld, max |delta| = %.6f\n",
                    what, firstDiff(ref, got), maxAbsDiff(ref, got));
    }
}
static void expectDifferent(const char* what, const std::vector<float>& ref,
                            const std::vector<float>& got) {
    if (!identical(ref, got)) {
        std::printf("  PASS  %s\n         differs as it must, max |delta| = %.6f\n",
                    what, maxAbsDiff(ref, got));
    } else {
        ++g_fail;
        std::printf("  FAIL  %s\n         IDENTICAL -- the automation did nothing, so the\n"
                    "         isolation above is vacuous\n", what);
    }
}

static void runIsolation() {
    std::printf("\n--- per-tracker-column FX isolation (bit-exact) ---\n");
    const double secs = 2.0;

    // (1) No column-1 voices exist at all.  Any divergence here is purely SHARED
    //     STATE: a column-addressed write that reached native_[] or a decimator
    //     phase/held sample that lives on the instrument instead of the voice.
    {
        Config ref; ref.playCol0 = true; ref.playCol1 = false; ref.autoOffset = false;
        Config aut = ref; aut.autoCol1 = true;
        std::vector<float> a, b;
        run(ref, secs, &a); run(aut, secs, &b);
        expectIdentical("column 0 audio, automating downsample+offset on column 1 "
                        "(no column-1 notes)", a, b);
    }

    // (2) Column 1 is genuinely PLAYING -- its voices run their own decimator and
    //     read pointer -- but muted to exact 0.0 so its own audio cannot change
    //     the sum.  Divergence here means column 1's voices perturbed column 0's.
    {
        Config ref; ref.playCol0 = true; ref.playCol1 = true; ref.autoOffset = false;
        ref.col1Gain = 0.f;
        Config aut = ref; aut.autoCol1 = true;
        std::vector<float> a, b;
        run(ref, secs, &a); run(aut, secs, &b);
        expectIdentical("column 0 audio, column 1 playing+muted, automating "
                        "downsample+offset on column 1", a, b);
    }

    // (3) The guard that stops (1) and (2) from being satisfied by an engine that
    //     simply ignores column-addressed writes: on column 1's OWN audio the
    //     same automation must change the output.
    {
        Config ref; ref.playCol0 = false; ref.playCol1 = true; ref.autoOffset = false;
        Config aut = ref; aut.autoCol1 = true;
        std::vector<float> a, b;
        run(ref, secs, &a); run(aut, secs, &b);
        expectDifferent("column 1 audio, automating downsample+offset on column 1", a, b);
    }
}

//============================================================================
//  PER-ZONE PARAMETER PARITY (SoundFont 2 generators)
//
//  Five properties, each of which the engine could plausibly get wrong in a way
//  that still looks wired up:
//
//   (A) FALLBACK.  A zone with no envelope of its own must use the
//       INSTRUMENT-global one.  Break it and every existing project loses its
//       amp envelope the moment per-zone envelopes exist.
//   (B) PER-ZONE RELEASE.  Two zones with different releases must release at
//       their own rates.  A single shared envelope passes every "is it wired
//       up" check and fails this one -- it is the whole reason for the work.
//   (C) EXCLUSIVE CLASS.  A note-on cuts sounding voices of the SAME class and
//       nothing else.  The failure that matters is over-reach: choking the
//       whole instrument, which would mute a drum kit's ride when the hat hits.
//   (D) PER-COLUMN ISOLATION UNDER RELEASE.  Releasing a voice in note-column 1
//       must not perturb column 0 by one bit -- per-zone envelope state has to
//       live on the VOICE, not on the zone, or two columns playing one zone
//       share (and fight over) one release.
//   (E) v4 BLOBS STILL LOAD, and v5 round-trips the new fields.
//============================================================================
struct ZEv { int frame; unsigned char status; int note; int vel; int column; };

//! Render a scripted note sequence and capture BOTH channels.  Deliberately
//! separate from run() above: these cases need the right channel (per-zone pan
//! is what separates one zone's audio from another's) and arbitrary event times.
static void zRender(IPluginInstance* inst, std::vector<ZEv> evs, int totalFrames,
                    std::vector<float>* outL, std::vector<float>* outR) {
    std::stable_sort(evs.begin(), evs.end(),
                     [](const ZEv& a, const ZEv& b){ return a.frame < b.frame; });
    std::vector<float> bufL((size_t)kBlock), bufR((size_t)kBlock);
    float* outs[2] = { bufL.data(), bufR.data() };
    if (outL) { outL->clear(); outL->reserve((size_t)totalFrames); }
    if (outR) { outR->clear(); outR->reserve((size_t)totalFrames); }
    size_t ei = 0;
    for (int start = 0; start < totalFrames; start += kBlock) {
        const int n = std::min(kBlock, totalFrames - start);
        MidiEvent mev[32];
        int ne = 0;
        while (ei < evs.size() && evs[ei].frame < start + n && ne < 32) {
            const ZEv& e = evs[ei++];
            MidiEvent& m = mev[ne++];
            m = MidiEvent{};
            m.sampleOffset = e.frame - start;
            m.status = e.status;
            m.data1 = (uint8_t)e.note;
            m.data2 = (uint8_t)e.vel;
            m.column = (int8_t)e.column;
        }
        ProcessBlock blk{};
        blk.audioIn = nullptr; blk.audioOut = outs; blk.nframes = n;
        blk.midiIn = mev; blk.numMidiIn = ne;
        blk.paramIn = nullptr; blk.numParamIn = 0;
        blk.tempoBpm = 120.0; blk.playPositionSamples = start; blk.isPlaying = true;
        blk.numAudioIn = 0; blk.numAudioOut = 2;
        inst->process(blk);
        for (int k = 0; k < n; ++k) {
            if (outL) outL->push_back(bufL[(size_t)k]);
            if (outR) outR->push_back(bufR[(size_t)k]);
        }
    }
}

static double rmsWindow(const std::vector<float>& x, double t0, double t1) {
    const size_t a = (size_t)(t0 * kSampleRate), b = (size_t)(t1 * kSampleRate);
    if (b <= a || b > x.size()) return 0.0;
    double s = 0.0;
    for (size_t i = a; i < b; ++i) s += (double)x[i] * (double)x[i];
    return std::sqrt(s / (double)(b - a));
}

static void expectTrue(const char* what, bool ok, const char* detail = nullptr) {
    if (ok) std::printf("  PASS  %s\n", what);
    else  { ++g_fail; std::printf("  FAIL  %s%s%s\n", what,
                                  detail ? "\n         " : "", detail ? detail : ""); }
}
static void expectCmp(const char* what, bool ok, double got, double want) {
    if (ok) std::printf("  PASS  %s  (%.5f vs %.5f)\n", what, got, want);
    else  { ++g_fail; std::printf("  FAIL  %s  (%.5f vs %.5f)\n", what, got, want); }
}

//! Load one sustaining looped zone.  `pcm` must outlive nothing -- the sampler
//! copies it -- so the caller may reuse one buffer for every zone.
static void loadZone(IPluginInstance* inst, int level, const std::vector<float>& pcm,
                     int root, int loKey, int hiKey) {
    sampler_load_sample_ex(inst, /*slot*/1, level, pcm.data(), (int)pcm.size(),
                           /*stereo*/false, root, (int)kSampleRate,
                           /*loopStart*/0, /*loopEnd*/(int)pcm.size(), /*loop*/true,
                           loKey, hiKey, /*loVel*/0, /*hiVel*/127,
                           /*noteOffLayer*/false, /*keyToPitch*/true,
                           /*velToVol*/true, /*overlapMode*/0, "zone");
}

//! An instrument-global amp envelope that falls to silence at 0.5 s.
//! (The global envelope's x axis spans 2 seconds, so x = 0.25 == 0.5 s.)
static void setGlobalDecayEnv(IPluginInstance* inst) {
    const unsigned short xs[2] = { 0, 16384 };
    const unsigned short ys[2] = { 65535, 0 };
    const int fl[2] = { 0, 0 };
    sampler_set_envelope(inst, /*env*/0, xs, ys, fl, 2);
}

static SamplerZoneEnv mkEnv(float a, float d, float s, float r) {
    SamplerZoneEnv e;
    e.delay = 0.f; e.attack = a; e.hold = 0.f; e.decay = d;
    e.sustain = s; e.release = r; e.enabled = 1;
    return e;
}

// --- (A) envelope fallback --------------------------------------------------
static void testEnvFallback(const std::vector<float>& pcm) {
    const int total = (int)(kSampleRate * 1.0);
    auto make = [&](bool globalEnv, bool zoneEnv) {
        IPluginInstance* inst = create_sampler_instrument();
        inst->prepare(kSampleRate, kBlock);
        loadZone(inst, 0, pcm, 60, 0, 127);
        if (globalEnv) setGlobalDecayEnv(inst);
        if (zoneEnv) {
            // sustain 1.0 forever: an override that is audibly NOT the global one
            SamplerZoneEnv e = mkEnv(0.f, 0.f, 1.f, 0.f);
            sampler_set_zone_env(inst, 1, 0, 0, &e);
        }
        std::vector<float> L;
        zRender(inst, { { 0, 0x90, 60, 100, -1 } }, total, &L, nullptr);
        inst->release(); delete inst;
        return rmsWindow(L, 0.6, 0.9);
    };
    const double bare      = make(false, false);
    const double fellBack  = make(true,  false);
    const double overridden= make(true,  true);

    expectCmp("no envelope anywhere -> zone sustains",
              bare > 0.05, bare, 0.05);
    expectCmp("zone with NO envelope falls back to the instrument's (silent "
              "after its decay)", fellBack < bare * 0.02, fellBack, bare);
    expectCmp("zone WITH its own envelope overrides the instrument's",
              overridden > bare * 0.5, overridden, bare);
}

// --- (B) per-zone release ---------------------------------------------------
static void testPerZoneRelease(const std::vector<float>& pcm) {
    const int total = (int)(kSampleRate * 1.6);
    IPluginInstance* inst = create_sampler_instrument();
    inst->prepare(kSampleRate, kBlock);
    loadZone(inst, 0, pcm, 48, 40, 59);     // "kick": 50 ms release
    loadZone(inst, 1, pcm, 72, 60, 79);     // "pad":  800 ms release
    SamplerZoneEnv fast = mkEnv(0.f, 0.f, 1.f, 0.05f);
    SamplerZoneEnv slow = mkEnv(0.f, 0.f, 1.f, 0.80f);
    sampler_set_zone_env(inst, 1, 0, 0, &fast);
    sampler_set_zone_env(inst, 1, 1, 0, &slow);

    const int offFrame = (int)(kSampleRate * 0.5);
    std::vector<float> shortL, longL;
    zRender(inst, { { 0, 0x90, 48, 100, -1 }, { offFrame, 0x80, 48, 0, -1 } },
            total, &shortL, nullptr);
    zRender(inst, { { 0, 0x90, 72, 100, -1 }, { offFrame, 0x80, 72, 0, -1 } },
            total, &longL, nullptr);

    const double shortHeld = rmsWindow(shortL, 0.2, 0.45);
    const double longHeld  = rmsWindow(longL,  0.2, 0.45);
    const double shortTail = rmsWindow(shortL, 0.7, 1.0);
    const double longTail  = rmsWindow(longL,  0.7, 1.0);

    expectCmp("both zones sustain while held", shortHeld > 0.05 && longHeld > 0.05,
              shortHeld, longHeld);
    expectCmp("50 ms-release zone is silent 200 ms after note-off",
              shortTail < shortHeld * 0.02, shortTail, shortHeld);
    expectCmp("800 ms-release zone is STILL sounding there -- the release is "
              "per zone, not per instrument", longTail > longHeld * 0.2,
              longTail, longHeld);
    // and it does end, at its own time
    expectCmp("...and it has ended by 1.4 s", rmsWindow(longL, 1.4, 1.55) < 1e-4,
              rmsWindow(longL, 1.4, 1.55), 1e-4);

    // read-back: the two zones report their own envelopes
    SamplerZoneEnv got0{}, got1{};
    const bool ok = sampler_get_zone_env(inst, 1, 0, 0, &got0) &&
                    sampler_get_zone_env(inst, 1, 1, 0, &got1);
    expectTrue("get_zone_env reports each zone's own release",
               ok && got0.enabled && got1.enabled &&
               std::fabs(got0.release - 0.05f) < 1e-6f &&
               std::fabs(got1.release - 0.80f) < 1e-6f);
    inst->release(); delete inst;
}

// --- (C) exclusive class ----------------------------------------------------
static void testExclusiveClass(const std::vector<float>& pcm) {
    const int total = (int)(kSampleRate * 1.6);
    // Zones A and B are hard LEFT, zone C hard RIGHT, so each side of the output
    // carries exactly one group and the choke is directly measurable.
    auto build = [&](int bClass) {
        IPluginInstance* inst = create_sampler_instrument();
        inst->prepare(kSampleRate, kBlock);
        loadZone(inst, 0, pcm, 48, 40, 49);   // A -- open hat
        loadZone(inst, 1, pcm, 50, 50, 59);   // B -- closed hat
        loadZone(inst, 2, pcm, 72, 70, 79);   // C -- an unrelated voice
        sampler_set_zone_level(inst, 1, 0, -1.f, 0.f);
        sampler_set_zone_level(inst, 1, 1, -1.f, 0.f);
        sampler_set_zone_level(inst, 1, 2, +1.f, 0.f);
        sampler_set_zone_exclusive(inst, 1, 0, 1);
        sampler_set_zone_exclusive(inst, 1, 1, bClass);
        sampler_set_zone_exclusive(inst, 1, 2, 0);
        return inst;
    };
    const int bFrame = (int)(kSampleRate * 1.0);
    std::vector<float> chokeL, chokeR, freeL, freeR, bOnlyL;

    IPluginInstance* i1 = build(1);           // B chokes A (same class)
    zRender(i1, { { 0, 0x90, 48, 100, -1 }, { 0, 0x90, 72, 100, -1 },
                  { bFrame, 0x90, 50, 100, -1 } }, total, &chokeL, &chokeR);
    i1->release(); delete i1;

    IPluginInstance* i2 = build(0);           // B has no class: nothing is cut
    zRender(i2, { { 0, 0x90, 48, 100, -1 }, { 0, 0x90, 72, 100, -1 },
                  { bFrame, 0x90, 50, 100, -1 } }, total, &freeL, &freeR);
    i2->release(); delete i2;

    IPluginInstance* i3 = build(1);           // reference: B alone on the left
    zRender(i3, { { 0, 0x90, 72, 100, -1 }, { bFrame, 0x90, 50, 100, -1 } },
            total, &bOnlyL, nullptr);
    i3->release(); delete i3;

    const double chokeAfter = rmsWindow(chokeL, 1.2, 1.5);
    const double freeAfter  = rmsWindow(freeL,  1.2, 1.5);
    const double bOnly      = rmsWindow(bOnlyL, 1.2, 1.5);

    expectCmp("same-class note-on CUTS the sounding voice (left ~= the new "
              "voice alone)", std::fabs(chokeAfter - bOnly) < bOnly * 0.05,
              chokeAfter, bOnly);
    expectCmp("...and without a class both voices keep sounding",
              freeAfter > chokeAfter * 1.15, freeAfter, chokeAfter);

    // The over-reach guard: the OTHER class (zone C, on the right) is untouched.
    const double cChoke = rmsWindow(chokeR, 1.2, 1.5);
    const double cBefore = rmsWindow(chokeR, 0.5, 0.8);
    expectCmp("a DIFFERENT class is not choked (right channel unchanged across "
              "the choke)", std::fabs(cChoke - cBefore) < cBefore * 0.02,
              cChoke, cBefore);
}

//! A choke must still be a FADE.  The trap: a zone whose own amp envelope has a
//! zero release (perfectly normal for a percussive zone) would, if the retired
//! tail kept stepping that envelope, be cut dead at whatever level it was at --
//! putting the click straight back exactly where a drum kit hits hardest.
static void testChokeIsDeclicked(const std::vector<float>& pcm) {
    const int total = (int)(kSampleRate * 1.6);
    IPluginInstance* inst = create_sampler_instrument();
    inst->prepare(kSampleRate, kBlock);
    loadZone(inst, 0, pcm, 48, 40, 49);
    loadZone(inst, 1, pcm, 50, 50, 59);
    SamplerZoneEnv e = mkEnv(0.f, 0.f, 1.f, 0.f);      // zero release
    sampler_set_zone_env(inst, 1, 0, 0, &e);
    sampler_set_zone_env(inst, 1, 1, 0, &e);
    sampler_set_zone_exclusive(inst, 1, 0, 3);
    sampler_set_zone_exclusive(inst, 1, 1, 3);
    std::vector<float> L;
    zRender(inst, { { 0, 0x90, 48, 100, -1 },
                    { (int)(kSampleRate * 1.0), 0x90, 50, 100, -1 } },
            total, &L, nullptr);
    inst->release(); delete inst;
    // widest single-sample step anywhere near the choke
    double worst = 0.0;
    const size_t a = (size_t)(kSampleRate * 0.95), b = (size_t)(kSampleRate * 1.10);
    for (size_t i = a + 1; i < b && i < L.size(); ++i)
        worst = std::max(worst, std::fabs((double)L[i] - (double)L[i - 1]));
    // two summed voices, so allow a couple of times the single-voice slew
    const double budget = contentSlew() * 4.0;
    expectCmp("choking a zero-release zone stays click-free (max step near the "
              "choke ~= content slew)", worst < budget, worst, budget);
}

// --- (D) per-column isolation under release ---------------------------------
static void testColumnReleaseIsolation(const std::vector<float>& pcm) {
    const int total = (int)(kSampleRate * 1.5);
    // Column 1 is muted to EXACT zero, so its voices genuinely run (own zone
    // envelope, own release) while contributing nothing to the sum -- which is
    // what makes a bit-exact comparison of column 0's audio meaningful.
    auto render = [&](int offNote, int offColumn, std::vector<float>& L) {
        IPluginInstance* inst = create_sampler_instrument();
        inst->prepare(kSampleRate, kBlock);
        loadZone(inst, 0, pcm, 60, 0, 127);
        SamplerZoneEnv e = mkEnv(0.f, 0.f, 1.f, 0.4f);
        sampler_set_zone_env(inst, 1, 0, 0, &e);
        sampler_set_column_param(inst, 1, (int)P_OUTPUT_GAIN, 0.f);
        std::vector<ZEv> evs = { { 0, 0x90, 60, 100, 0 }, { 0, 0x90, 64, 100, 1 } };
        if (offNote > 0)
            evs.push_back({ (int)(kSampleRate * 0.5), 0x80, offNote, 0, offColumn });
        zRender(inst, evs, total, &L, nullptr);
        inst->release(); delete inst;
    };
    std::vector<float> ref, col1Off, col0Off;
    render(0,  -1, ref);
    render(64,  1, col1Off);
    render(60,  0, col0Off);
    expectIdentical("column 0 audio, releasing column 1's voice mid-note "
                    "(per-zone envelope)", ref, col1Off);
    expectDifferent("column 0 audio, releasing column 0's OWN voice", ref, col0Off);
}

// --- (E) state blob: v4 still loads, v5 round-trips -------------------------
namespace blob {
static void p32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (i * 8)));
}
static void pf(std::vector<uint8_t>& b, float f) {
    uint32_t v; std::memcpy(&v, &f, 4); p32(b, v);
}
} // namespace blob

//! Hand-built legacy blob (version 4 or 5): one zone, one instrument-global amp
//! envelope.  Written byte-by-byte rather than captured from saveState() on
//! purpose -- saveState() now emits v6, so only a literal older image proves the
//! older paths still load.  v5 adds the per-zone parameter tail; neither has the
//! v6 share index, so both must be read with the PCM inline.
static std::vector<uint8_t> makeLegacyBlob(const std::vector<float>& pcm, uint32_t ver) {
    using blob::p32; using blob::pf;
    std::vector<uint8_t> b;
    p32(b, 0x53504d53u);            // "SMPS"
    p32(b, ver);
    // The engine's own parameter defaults, as a real v4 project would carry
    // them (0.5 across the board would key-range the instrument down to a
    // single note and leave the noise generator wide open).
    static const float kNative[28] = {
        0.5f,0.5f,0.5f,0.f,1.f,0.f,1.f,0.f,0.5f,1.f,0.f,1.f,0.f,0.f,0.f,0.f,
        0.f,0.3f,0.5f,0.f,0.f,0.5f,0.f,0.f,
        0.4f,0.f,0.f,0.2f };
    const uint32_t nativeCount = 28;
    p32(b, nativeCount); p32(b, 0); // nativeCount, buzzCount
    for (uint32_t i = 0; i < nativeCount; ++i) pf(b, kNative[i]);
    p32(b, 1);                      // one zone
    p32(b, 1); p32(b, 0);           // slot, level
    p32(b, 55); p32(b, 20); p32(b, 90);          // root, loKey, hiKey
    p32(b, 10); p32(b, 120);                     // loVel, hiVel
    p32(b, 0); p32(b, 1); p32(b, 1); p32(b, 2);  // noteOffLayer,k2p,v2v,overlap
    p32(b, (uint32_t)kSampleRate); p32(b, 0); p32(b, (uint32_t)pcm.size());
    p32(b, 1); p32(b, 0);                        // loop, stereo
    p32(b, (uint32_t)pcm.size());                // numFrames
    const char* nm = "v4zone";
    p32(b, 6); for (int i = 0; i < 6; ++i) b.push_back((uint8_t)nm[i]);
    p32(b, (uint32_t)pcm.size());
    for (float s : pcm) pf(b, s);
    if (ver >= 5) {                              // v5 per-zone tail
        for (int e = 0; e < 2; ++e) {            // ampEnv, modEnv: both disabled
            p32(b, 0);
            for (int k = 0; k < 6; ++k) pf(b, 0.f);
        }
        pf(b, 0.f); pf(b, 0.f);                  // cutoff, resonance
        p32(b, 0); p32(b, 0); p32(b, 100);       // coarse, fine, scaleTuning
        pf(b, 0.f); pf(b, 0.f);                  // pan, attenuation
        p32(b, 0);                               // exclusiveClass
        pf(b, 0.f); pf(b, 0.f);                  // modEnv routes
    }
    p32(b, 5);                                   // kNumEnvs
    for (int e = 0; e < 5; ++e) {
        if (e == 0) {
            p32(b, 2);
            pf(b, 0.f);   pf(b, 1.f); p32(b, 0);
            pf(b, 0.25f); pf(b, 0.f); p32(b, 0);
        } else {
            p32(b, 0);
        }
    }
    return b;
}

static void testStateBlobs(const std::vector<float>& pcm) {
    // --- v4 and v5 ----------------------------------------------------------
    for (uint32_t ver : { 4u, 5u }) {
        char what[128];
        IPluginInstance* inst = create_sampler_instrument();
        inst->prepare(kSampleRate, kBlock);
        inst->loadState(makeLegacyBlob(pcm, ver));
        SamplerZoneInfo z;
        const bool got = sampler_zone_count(inst) == 1 &&
                         sampler_get_zone_meta(inst, 0, z);
        std::snprintf(what, sizeof what, "a v%u blob still loads its zone", ver);
        expectTrue(what,
                   got && z.rootKey == 55 && z.loKey == 20 && z.hiKey == 90 &&
                   z.loVel == 10 && z.hiVel == 120 && z.overlapMode == 2 &&
                   z.loop && z.name == "v4zone" &&
                   z.numFrames == (int)pcm.size());
        std::snprintf(what, sizeof what,
                      "a v%u blob still loads its instrument envelope", ver);
        expectTrue(what, sampler_has_envelopes(inst));
        std::snprintf(what, sizeof what,
                      "a v%u zone defaults to NO per-zone override (falls back)", ver);
        expectTrue(what,
                   got && z.ampEnv.enabled == 0 && z.modEnv.enabled == 0 &&
                   z.scaleTuning == 100 && z.exclusiveClass == 0 &&
                   z.cutoffHz == 0.f);
        // and it still plays -- through the instrument envelope it restored
        std::vector<float> L;
        zRender(inst, { { 0, 0x90, 55, 100, -1 } }, (int)(kSampleRate * 1.0), &L, nullptr);
        const double held = rmsWindow(L, 0.1, 0.3), tail = rmsWindow(L, 0.7, 0.9);
        std::snprintf(what, sizeof what,
                      "a v%u blob's restored instrument envelope still shapes "
                      "the zone", ver);
        expectCmp(what, held > 0.02 && tail < held * 0.02, tail, held);
        inst->release(); delete inst;
    }
    // --- current-version round-trip -----------------------------------------
    {
        IPluginInstance* src = create_sampler_instrument();
        src->prepare(kSampleRate, kBlock);
        loadZone(src, 0, pcm, 48, 40, 59);
        SamplerZoneEnv amp = mkEnv(0.01f, 0.2f, 0.6f, 0.35f);
        SamplerZoneEnv mod = mkEnv(0.02f, 0.3f, 0.4f, 0.15f);
        sampler_set_zone_env(src, 1, 0, 0, &amp);
        sampler_set_zone_env(src, 1, 0, 1, &mod);
        sampler_set_zone_filter(src, 1, 0, 1234.f, 6.f);
        sampler_set_zone_tuning(src, 1, 0, -3, 27, 0);
        sampler_set_zone_level(src, 1, 0, -0.5f, 3.5f);
        sampler_set_zone_exclusive(src, 1, 0, 7);
        sampler_set_zone_modroute(src, 1, 0, 400.f, -800.f);
        std::vector<uint8_t> st = src->saveState();
        src->release(); delete src;

        IPluginInstance* dst = create_sampler_instrument();
        dst->prepare(kSampleRate, kBlock);
        dst->loadState(st);
        SamplerZoneInfo z;
        const bool got = sampler_zone_count(dst) == 1 && sampler_get_zone_meta(dst, 0, z);
        expectTrue("the current blob round-trips every per-zone field",
                   got &&
                   z.ampEnv.enabled && std::fabs(z.ampEnv.release - 0.35f) < 1e-6f &&
                   std::fabs(z.ampEnv.sustain - 0.6f) < 1e-6f &&
                   z.modEnv.enabled && std::fabs(z.modEnv.decay - 0.3f) < 1e-6f &&
                   std::fabs(z.cutoffHz - 1234.f) < 1e-3f &&
                   std::fabs(z.resonanceDb - 6.f) < 1e-6f &&
                   z.coarseTune == -3 && z.fineTune == 27 && z.scaleTuning == 0 &&
                   std::fabs(z.pan + 0.5f) < 1e-6f &&
                   std::fabs(z.attenuationDb - 3.5f) < 1e-6f &&
                   z.exclusiveClass == 7 &&
                   std::fabs(z.modEnvToPitchCents - 400.f) < 1e-3f &&
                   std::fabs(z.modEnvToFilterCents + 800.f) < 1e-3f);
        // scaleTuning 0 == fixed pitch: every key in the zone plays the same
        // rate, which is what a drum zone needs.
        std::vector<float> a, b2;
        zRender(dst, { { 0, 0x90, 45, 100, -1 } }, (int)(kSampleRate * 0.4), &a, nullptr);
        IPluginInstance* dst2 = create_sampler_instrument();
        dst2->prepare(kSampleRate, kBlock);
        dst2->loadState(st);
        zRender(dst2, { { 0, 0x90, 55, 100, -1 } }, (int)(kSampleRate * 0.4), &b2, nullptr);
        expectIdentical("scaleTuning 0 makes the zone fixed-pitch across keys", a, b2);
        dst2->release(); delete dst2;
        dst->release(); delete dst;
    }
}


//============================================================================
//  SHARED PCM BUFFERS
//
//  Multisampled instruments reuse one recording across velocity layers and
//  round-robins, so zones sharing a sample is the NORMAL case.  Measured on a
//  real 798 MB bank, one Concert Grand preset is 2976 zones over 192 distinct
//  samples: ~85 MB of audio that costs ~1.3 GB if each zone owns a copy.
//
//  Four things have to hold, and each has a distinct failure mode:
//    * sharing actually happens (same allocation, not merely equal bytes);
//    * a shared zone renders IDENTICALLY to a copied one -- otherwise the
//      optimisation changes the sound, which is not an optimisation;
//    * replacing one sharer's sample leaves the others playing -- the trap is
//      freeing a buffer somebody else is still reading;
//    * the project file dedupes too, or the 1.3 GB simply moves into the blob.
//============================================================================
static PatchKnob::engine::SharedPcm intern(const std::vector<float>& v) {
    return std::make_shared<const std::vector<float>>(v);
}

//! Distinct PCM allocations across an instrument's zones, and the bytes they
//! occupy.  Counted by POINTER IDENTITY: two zones that merely happen to hold
//! equal audio in two allocations are two allocations, and must be counted so.
static size_t distinctBuffers(IPluginInstance* inst, size_t bytesEach, size_t* outBytes) {
    std::vector<const float*> seen;
    const int n = sampler_zone_count(inst);
    for (int i = 0; i < n; ++i) {
        const float* p = sampler_zone_pcm_ptr(inst, i);
        if (!p) continue;
        if (std::find(seen.begin(), seen.end(), p) == seen.end()) seen.push_back(p);
    }
    if (outBytes) *outBytes = seen.size() * bytesEach;
    return seen.size();
}

// --- (F) zones share one allocation -----------------------------------------
static void testSharedBuffers(const std::vector<float>& pcm) {
    PatchKnob::engine::SharedPcm buf = intern(pcm);
    IPluginInstance* inst = create_sampler_instrument();
    inst->prepare(kSampleRate, kBlock);
    auto add = [&](int level, int root, int lo, int hi) {
        sampler_load_sample_shared(inst, 1, level, buf, (int)pcm.size(), false, root,
                                   (int)kSampleRate, 0, (int)pcm.size(), true,
                                   lo, hi, 0, 127, false, true, true, 0, "shared");
    };
    add(0, 48, 40, 49);
    add(1, 50, 50, 59);
    add(2, 72, 70, 79);
    const float* p0 = sampler_zone_pcm_ptr(inst, 0);
    expectTrue("three zones from one shared buffer are ONE allocation",
               p0 && p0 == buf->data() &&
               p0 == sampler_zone_pcm_ptr(inst, 1) &&
               p0 == sampler_zone_pcm_ptr(inst, 2));

    // ...and the copying path still gives every zone its own, as it always did.
    IPluginInstance* cp = create_sampler_instrument();
    cp->prepare(kSampleRate, kBlock);
    loadZone(cp, 0, pcm, 48, 40, 49);
    loadZone(cp, 1, pcm, 50, 50, 59);
    expectTrue("the copying path is unchanged: two zones, two allocations",
               sampler_zone_pcm_ptr(cp, 0) != sampler_zone_pcm_ptr(cp, 1) &&
               sampler_zone_pcm_ptr(cp, 0) != buf->data());
    cp->release(); delete cp;
    inst->release(); delete inst;
}

// --- (G) shared audio is bit-identical to copied audio ----------------------
static void testSharedRendersIdentically(const std::vector<float>& pcm) {
    const int total = (int)(kSampleRate * 1.0);
    const std::vector<ZEv> script = { { 0, 0x90, 60, 100, -1 },
                                      { (int)(kSampleRate * 0.4), 0x80, 60, 0, -1 },
                                      { (int)(kSampleRate * 0.5), 0x90, 67, 90, -1 } };
    std::vector<float> copied, shared;
    {
        IPluginInstance* inst = create_sampler_instrument();
        inst->prepare(kSampleRate, kBlock);
        loadZone(inst, 0, pcm, 60, 0, 127);
        zRender(inst, script, total, &copied, nullptr);
        inst->release(); delete inst;
    }
    {
        IPluginInstance* inst = create_sampler_instrument();
        inst->prepare(kSampleRate, kBlock);
        sampler_load_sample_shared(inst, 1, 0, intern(pcm), (int)pcm.size(), false, 60,
                                   (int)kSampleRate, 0, (int)pcm.size(), true,
                                   0, 127, 0, 127, false, true, true, 0, "zone");
        zRender(inst, script, total, &shared, nullptr);
        inst->release(); delete inst;
    }
    expectIdentical("a shared-buffer zone renders bit-identically to a copied one",
                    copied, shared);
}

// --- (H) replacing one sharer's sample leaves the others alone --------------
static void testReplaceOneSharer(const std::vector<float>& pcm) {
    const int total = (int)(kSampleRate * 0.5);
    // A different waveform, so "still playing the ORIGINAL audio" is provable
    // rather than merely "still playing something".
    std::vector<float> other((size_t)kSampleRate);
    for (size_t i = 0; i < other.size(); ++i)
        other[i] = (float)(0.4 * std::sin(6.283185307 * 90.0 * (double)i / kSampleRate));

    PatchKnob::engine::SharedPcm buf = intern(pcm);
    auto build = [&]() {
        IPluginInstance* inst = create_sampler_instrument();
        inst->prepare(kSampleRate, kBlock);
        for (int lv = 0; lv < 2; ++lv)
            sampler_load_sample_shared(inst, 1, lv, buf, (int)pcm.size(), false,
                                       lv == 0 ? 48 : 72, (int)kSampleRate,
                                       0, (int)pcm.size(), true,
                                       lv == 0 ? 40 : 70, lv == 0 ? 49 : 79,
                                       0, 127, false, true, true, 0, "shared");
        return inst;
    };
    std::vector<float> before, after;
    IPluginInstance* a = build();
    zRender(a, { { 0, 0x90, 72, 100, -1 } }, total, &before, nullptr);
    a->release(); delete a;

    IPluginInstance* b = build();
    // Swap the OTHER zone's sample for unrelated audio -- level 0 drops its
    // reference to `buf`, level 1 keeps it.
    loadZone(b, 0, other, 48, 40, 49);
    const bool stillShared = sampler_zone_pcm_ptr(b, 1) == buf->data();
    zRender(b, { { 0, 0x90, 72, 100, -1 } }, total, &after, nullptr);
    b->release(); delete b;

    expectTrue("the surviving zone still points at the original buffer", stillShared);
    expectIdentical("replacing one sharer's sample does not disturb the other",
                    before, after);
}

// --- (I) the project blob dedupes too ---------------------------------------
static void testSharedBlobRoundTrip(const std::vector<float>& pcm) {
    PatchKnob::engine::SharedPcm b1 = intern(pcm);
    std::vector<float> alt(pcm.begin(), pcm.begin() + pcm.size() / 2);
    PatchKnob::engine::SharedPcm b2 = intern(alt);

    IPluginInstance* src = create_sampler_instrument();
    src->prepare(kSampleRate, kBlock);
    for (int lv = 0; lv < 8; ++lv) {
        const PatchKnob::engine::SharedPcm& b = (lv < 5) ? b1 : b2;
        sampler_load_sample_shared(src, 1, lv, b, (int)b->size(), false, 60 + lv,
                                   (int)kSampleRate, 0, (int)b->size(), true,
                                   lv * 8, lv * 8 + 7, 0, 127,
                                   false, true, true, 0, "shared");
    }
    std::vector<uint8_t> st = src->saveState();
    std::vector<float> before;
    zRender(src, { { 0, 0x90, 4, 100, -1 } }, (int)(kSampleRate * 0.3), &before, nullptr);
    src->release(); delete src;

    // Eight zones, two buffers: the blob must carry the audio twice, not eight
    // times.  Compared against the copying layout rather than a magic constant.
    const size_t audioOnce = (pcm.size() + alt.size()) * 4;
    const size_t audioEight = (pcm.size() * 5 + alt.size() * 3) * 4;
    expectCmp("saveState writes each distinct buffer ONCE (blob ~= 2 buffers, "
              "not 8)", st.size() < audioOnce + 8192 && st.size() < audioEight / 3,
              (double)st.size(), (double)audioEight);

    IPluginInstance* dst = create_sampler_instrument();
    dst->prepare(kSampleRate, kBlock);
    dst->loadState(st);
    size_t bytes = 0;
    const size_t nd = distinctBuffers(dst, 1, &bytes);
    expectCmp("loadState RE-SHARES: 8 zones restore to 2 allocations",
              sampler_zone_count(dst) == 8 && nd == 2, (double)nd, 2.0);
    std::vector<float> after;
    zRender(dst, { { 0, 0x90, 4, 100, -1 } }, (int)(kSampleRate * 0.3), &after, nullptr);
    expectIdentical("audio survives a shared-buffer save/load round-trip", before, after);
    dst->release(); delete dst;
}

// --- (J) the headline number ------------------------------------------------
//! The measured case from the bug report: a Concert Grand preset, 2976 zones
//! drawing on 192 distinct samples.  Built here at a reduced per-sample size so
//! the test runs in a second; the RATIO is what the shape of the preset fixes,
//! and it is the ratio that turns 85 MB into 1.3 GB at full size.
static void reportBankSaving() {
    const int kZones = 2976, kSamples = 192, kFrames = 4096;
    const size_t bytesEach = (size_t)kFrames * sizeof(float);

    std::vector<PatchKnob::engine::SharedPcm> bank;
    bank.reserve(kSamples);
    std::vector<float> tmp((size_t)kFrames);
    for (int i = 0; i < kSamples; ++i) {
        for (int k = 0; k < kFrames; ++k)
            tmp[(size_t)k] = (float)std::sin(6.283185307 * (60.0 + i) * k / kSampleRate);
        bank.push_back(intern(tmp));
    }
    auto fill = [&](IPluginInstance* inst, bool shared) {
        for (int zi = 0; zi < kZones; ++zi) {
            const int slot = 1 + zi / 128, level = zi % 128;
            const PatchKnob::engine::SharedPcm& b = bank[(size_t)(zi % kSamples)];
            if (shared)
                sampler_load_sample_shared(inst, slot, level, b, kFrames, false, 60,
                                           (int)kSampleRate, 0, kFrames, true,
                                           0, 127, 0, 127, false, true, true, 0, "z");
            else
                sampler_load_sample_ex(inst, slot, level, b->data(), kFrames, false, 60,
                                       (int)kSampleRate, 0, kFrames, true,
                                       0, 127, 0, 127, false, true, true, 0, "z");
        }
    };
    size_t sharedBytes = 0, copiedBytes = 0;
    size_t sharedN = 0, copiedN = 0;
    {
        IPluginInstance* inst = create_sampler_instrument();
        inst->prepare(kSampleRate, kBlock);
        fill(inst, true);
        sharedN = distinctBuffers(inst, bytesEach, &sharedBytes);
        inst->release(); delete inst;
    }
    {
        IPluginInstance* inst = create_sampler_instrument();
        inst->prepare(kSampleRate, kBlock);
        fill(inst, false);
        copiedN = distinctBuffers(inst, bytesEach, &copiedBytes);
        inst->release(); delete inst;
    }
    const double mbS = (double)sharedBytes / (1024.0 * 1024.0);
    const double mbC = (double)copiedBytes / (1024.0 * 1024.0);
    std::printf("  %d zones over %d distinct samples (%zu KB each):\n",
                kZones, kSamples, bytesEach / 1024);
    std::printf("      copying path : %6zu allocations, %8.1f MB\n", copiedN, mbC);
    std::printf("      shared path  : %6zu allocations, %8.1f MB   (%.1fx less)\n",
                sharedN, mbS, mbC / (mbS > 0.0 ? mbS : 1.0));
    std::printf("      at the real bank's ~442 KB/sample that is %.0f MB -> %.0f MB\n",
                mbC * 442.0 / ((double)bytesEach / 1024.0),
                mbS * 442.0 / ((double)bytesEach / 1024.0));
    expectCmp("2976 zones over 192 samples hold 192 allocations",
              sharedN == (size_t)kSamples && copiedN == (size_t)kZones,
              (double)sharedN, (double)kSamples);
}

static void runZoneParity() {
    std::printf("\n--- per-zone parameters (SF2 generator parity) ---\n");
    const std::vector<float> pcm = makeSample((int)kSampleRate);
    testEnvFallback(pcm);
    testPerZoneRelease(pcm);
    testExclusiveClass(pcm);
    testChokeIsDeclicked(pcm);
    testColumnReleaseIsolation(pcm);
    testStateBlobs(pcm);
}

static void runSharedPcm() {
    std::printf("\n--- shared PCM buffers ---\n");
    const std::vector<float> pcm = makeSample((int)kSampleRate);
    testSharedBuffers(pcm);
    testSharedRendersIdentically(pcm);
    testReplaceOneSharer(pcm);
    testSharedBlobRoundTrip(pcm);
    reportBankSaving();
}

int main(int argc, char** argv) {
    const bool post = (argc > 1 && std::strcmp(argv[1], "--post") == 0);
    std::printf("sampler click measurement -- 128th notes @120BPM (a note every "
                "%d samples) + automated Sample Offset\n", kNotePeriod);
    std::printf("source material max natural slew = %.4f / sample\n\n", contentSlew());

    if (post) {
        Config c; c.declick = 0.f; c.offsetMode = 0.f; c.zeroSnap = 0.f;
        report("declick OFF (pre-fix behaviour)", run(c));

        Config d; d.offsetMode = 0.f; d.zeroSnap = 0.f;
        report("declick ON  (DEFAULT, clean)", run(d));

        Config z; z.offsetMode = 0.f; z.zeroSnap = 0.5f;
        report("declick ON + zero-cross snap", run(z));

        Config a; a.offsetMode = 1.f; a.zeroSnap = 0.f;
        report("AKAI smear mode", run(a));

        Config n; n.offsetMode = 0.f; n.noteOff = true;
        report("declick ON, with note-offs", run(n));

        Config u; u.offsetMode = 0.f; u.column = false;
        report("declick ON, untagged (32-voice steal)", run(u));
    } else {
        Config c;              report("ORIGINAL engine",                  run(c));
        Config u; u.column = false;
        report("ORIGINAL, untagged (32-voice steal)", run(u));
        Config n; n.noteOff = true;
        report("ORIGINAL, with note-offs",            run(n));
    }

    runIsolation();
    runZoneParity();
    runSharedPcm();
    std::printf("\n%s\n", g_fail ? "*** FAILURES ***" : "all checks passed");
    return g_fail ? 1 : 0;
}
