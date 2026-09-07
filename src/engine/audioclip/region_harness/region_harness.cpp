//----------------------------------------------------------------------------
//  region_harness.cpp -- head-less proof of the audio_app PROJECT-REGION layer
//  under the exact call sequence the arrange shell performs for a clip SPLIT:
//
//      right half : audio_app_freeze_attach_shared + audio_app_freeze_set_region
//      left half  : audio_app_project_set_region(track, clip, ...)
//
//  The assertions are about what the ENGINE actually ends up scheduling
//  (region count / start / offset / length via the region-id enumeration API)
//  and about the RENDERED SAMPLES (audio_app_freeze_render_at through the real
//  playback path) -- not about any view-side bookkeeping, which can look
//  correct while playback is not.
//
//  The source clip is a step signal (left half 0.25, right half 0.75 on both
//  channels), so "which source material is audible at timeline sample s" is a
//  single amplitude probe.
//
//  No GUI.  The audio device is opened by audio_app_init() and immediately
//  stopped; every render is pumped offline by freeze_render_at itself.
//----------------------------------------------------------------------------
#include "audio_app.h"
#include "engine/audio/audio_engine.h"
#include "engine/audioclip/audio_clip.h"
#include "engine/audioclip/consolidate.h"
#include "engine/audioclip/warp_stretch.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

// audio_app.cpp references the metronome click sample, which the real app
// build generates from Click.wav at configure time.  The engine is never
// asked to click here, so an empty blob satisfies the linker.
extern const unsigned char g_click_wav[1];
extern const std::size_t   g_click_wav_size;
const unsigned char g_click_wav[1] = { 0 };
const std::size_t   g_click_wav_size = 0;

using namespace PatchKnob::app;
using PatchKnob::engine::ConsolidatePiece;
using PatchKnob::engine::consolidateRender;
using PatchKnob::engine::AudioClip;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) std::printf("PASS  %s\n", msg); \
    else      { std::printf("FAIL  %s\n", msg); ++g_fail; } \
} while (0)

// Mean |sample| of the rendered left channel over [a, b) -- amplitude probe.
static double probe(const AudioClip& c, long long a, long long b)
{
    if (a < 0) a = 0;
    if (b > (long long)c.ch[0].size()) b = (long long)c.ch[0].size();
    if (b <= a) return -1.0;
    double acc = 0.0;
    for (long long i = a; i < b; ++i) acc += std::fabs(c.ch[0][(size_t)i]);
    return acc / (double)(b - a);
}
// The master strip pans center with an equal-power law, so a unity-gain clip
// renders at 1/sqrt(2) of its stored amplitude on each channel.
static const double kPan = 0.70710678;
static bool near_to(double v, double want, double tol) { return std::fabs(v - want) <= tol; }


//----------------------------------------------------------------------------
//  SAMPLE-EXACT alignment probe (section 10).
//
//  The step source above answers "which HALF of the file is audible"; it cannot
//  see a slice that is a few samples out.  A sawtooth whose value encodes its
//  own sample index modulo a prime period answers "WHICH SOURCE SAMPLE is
//  audible", so a piece's alignment can be measured rather than inferred: slide
//  the expected material by a few samples and see which shift the rendered
//  audio actually agrees with.  Zero must win, and win clearly.
//
//  Prime period: 97 shares no factor with the block size, so no shift can be
//  masked by a block boundary.  Correlation, not difference, so the master
//  strip's pan/gain law cancels out of the measurement.
//----------------------------------------------------------------------------
static const long long kSawPeriod = 97;
static double saw(long long i)
{
    long long m = i % kSawPeriod;
    if (m < 0) m += kSawPeriod;
    return 0.1 + 0.8 * (double)m / (double)(kSawPeriod - 1);
}

//  Pearson correlation between the render over [a,b) and the source material a
//  region {start, off} should be putting there, slid by `shift` samples.
static double corr_at(const AudioClip& out, long long start, long long off,
                      long long a, long long b, long long shift)
{
    if (a < 0) a = 0;
    if (b > (long long)out.ch[0].size()) b = (long long)out.ch[0].size();
    if (b - a < 64) return -2.0;
    const double n = (double)(b - a);
    double sx = 0, sy = 0;
    for (long long s = a; s < b; ++s) {
        sx += out.ch[0][(size_t)s];
        sy += saw(s - start + off + shift);
    }
    const double mx = sx / n, my = sy / n;
    double sxy = 0, sxx = 0, syy = 0;
    for (long long s = a; s < b; ++s) {
        const double dx = out.ch[0][(size_t)s] - mx;
        const double dy = saw(s - start + off + shift) - my;
        sxy += dx * dy; sxx += dx * dx; syy += dy * dy;
    }
    if (sxx <= 0 || syy <= 0) return -2.0;
    return sxy / std::sqrt(sxx * syy);
}

//  The shift the rendered audio agrees with best, plus the best rival, so a
//  "0 wins" result can be shown to be decisive rather than a coin toss.
static long long best_shift(const AudioClip& out, long long start, long long off,
                            long long a, long long b, double* bestR, double* rivalR)
{
    long long best = 0; double br = -2.0, rr = -2.0;
    for (long long k = -8; k <= 8; ++k) {
        const double r = corr_at(out, start, off, a, b, k);
        if (r > br) { rr = br; br = r; best = k; }
        else if (r > rr) rr = r;
    }
    if (bestR)  *bestR  = br;
    if (rivalR) *rivalR = rr;
    return best;
}

int main()
{
    if (!audio_app_init()) {
        std::printf("FAIL  audio_app_init (no output device available)\n");
        return 1;
    }
    // Deterministic offline pumping: freeze_render_at drives the transport and
    // render blocks itself; with the stream stopped nothing runs concurrently.
    if (audio_app_engine()) audio_app_engine()->stop();

    const double sr = audio_app_sample_rate();
    const long long L = (long long)sr * 2;            // 2 s source
    const long long X = L / 2;                        // split point = 1 s
    const long long G = (long long)(sr * 0.25);       // gap when moving the right half

    // Step source: 0.25 for the first half, 0.75 for the second.
    AudioClip src;
    src.name = "step";
    src.sampleRate = src.sourceSampleRate = sr;
    src.resize(L);
    for (long long i = 0; i < L; ++i) {
        const float v = i < X ? 0.25f : 0.75f;
        src.ch[0][(size_t)i] = v;
        src.ch[1][(size_t)i] = v;
    }

    const int track = audio_app_master_add_track(0);
    CHECK(track >= 0, "mixer track allocated");

    // ---- 1. schedule the whole clip (the WAV-drop / record path) -----------
    CHECK(audio_app_project_add_audio_clip(track, src, 0, 1.0f),
          "add_audio_clip stores and schedules the source (no crash)");
    const AudioClip* stored = audio_app_project_clip_on_track(track);
    CHECK(stored != nullptr, "stored clip is reachable on the track");
    CHECK(audio_app_project_region_count(track) == 1, "one region scheduled");
    const unsigned long long leftId = audio_app_project_last_region_id(track);
    CHECK(leftId != 0, "the scheduled region has a stable id");

    // ---- 2. the SPLIT, exactly as the shell wires it ------------------------
    // Right half first (create_pattern -> on_clip_duplicated), then the left
    // trim (on_clip_region_changed fallback path).
    const int fz = audio_app_freeze_attach_shared(track, stored, X, 1.0f);
    CHECK(fz >= 0, "right half attaches as a shared region (freeze id)");
    CHECK(audio_app_freeze_set_region(fz, X, X, L - X),
          "right half region set to {P+B, S+B, L-B}");
    CHECK(audio_app_project_set_region(track, stored, 0, 0, X),
          "left half trimmed to {P, S, B} through the project API");

    // ---- 3. what the engine actually holds ----------------------------------
    CHECK(audio_app_project_region_count(track) == 2,
          "engine holds TWO regions after the split");
    long long p0=0,o0=0,l0=0, p1=0,o1=0,l1=0;
    const unsigned long long id0 = audio_app_project_region_id_at(track, 0);
    const unsigned long long id1 = audio_app_project_region_id_at(track, 1);
    CHECK(id0 != 0 && id1 != 0 && id0 != id1, "the two regions have distinct ids");
    CHECK(audio_app_project_find_region_by_id(track, id0, &p0, &o0, &l0) &&
          audio_app_project_find_region_by_id(track, id1, &p1, &o1, &l1),
          "both regions are addressable by id");
    std::printf("      region0 id=%llu {start=%lld off=%lld len=%lld}\n", id0, p0, o0, l0);
    std::printf("      region1 id=%llu {start=%lld off=%lld len=%lld}\n", id1, p1, o1, l1);
    CHECK(p0 == 0 && o0 == 0 && l0 == X, "left region is {0, 0, B}");
    CHECK(p1 == X && o1 == X && l1 == L - X, "right region is {B, B, L-B}");

    // ---- 4. rendered audio: the cut must be inaudible in place --------------
    const double q = sr * 0.1;    // probe window: 100 ms, away from declick edges
    AudioClip out;
    CHECK(audio_app_freeze_render_at(track, 0, (double)L / sr + 0.2,
                                     std::function<void()>(),
                                     std::function<void(long long,long long)>(), out),
          "offline render through the real playback path succeeds");
    std::printf("      render frames=%lld probeL=%.4f probeR=%.4f\n",
                (long long)out.numFrames(),
                probe(out, (long long)(X/2 - q), (long long)(X/2 + q)),
                probe(out, (long long)(X + X/2 - q), (long long)(X + X/2 + q)));
    CHECK(near_to(probe(out, (long long)(X/2 - q), (long long)(X/2 + q)), 0.25 * kPan, 0.02),
          "left half renders the LEFT source material (0.25)");
    CHECK(near_to(probe(out, (long long)(X + X/2 - q), (long long)(X + X/2 + q)), 0.75 * kPan, 0.02),
          "right half renders the RIGHT source material (0.75)");

    // ---- 5. the cut is REAL: move the right half later -> a silent gap ------
    CHECK(audio_app_freeze_set_region(fz, X + G, X, L - X),
          "right half moves later by 0.25 s");
    AudioClip out2;
    CHECK(audio_app_freeze_render_at(track, 0, (double)(L + G) / sr + 0.2,
                                     std::function<void()>(),
                                     std::function<void(long long,long long)>(), out2),
          "re-render after the move succeeds");
    CHECK(near_to(probe(out2, (long long)(X - 2*q), (long long)(X - q)), 0.25 * kPan, 0.02),
          "left half still ends with LEFT material just before the cut");
    CHECK(near_to(probe(out2, (long long)(X + q/4), (long long)(X + G - q/4)), 0.0, 0.005),
          "the gap left by the moved right half is SILENT (the cut is real)");
    CHECK(near_to(probe(out2, (long long)(X + G + q), (long long)(X + G + 3*q)), 0.75 * kPan, 0.02),
          "moved right half still plays RIGHT material from source offset B");

    // ---- 6. edits after the split keep addressing the correct piece ---------
    CHECK(audio_app_project_set_region_by_id(track, leftId, 0, 0, X / 2),
          "left half re-trims by id");
    long long lp=0, lo=0, ll=0;
    audio_app_project_find_region_by_id(track, leftId, &lp, &lo, &ll);
    CHECK(lp == 0 && lo == 0 && ll == X / 2, "left region is now {0, 0, B/2}");
    long long rp=0, ro=0, rl=0;
    audio_app_project_find_region_by_id(track, id1, &rp, &ro, &rl);
    CHECK(rp == X + G && ro == X && rl == L - X,
          "right region untouched by the left edit");
    AudioClip out3;
    CHECK(audio_app_freeze_render_at(track, 0, (double)(L + G) / sr + 0.2,
                                     std::function<void()>(),
                                     std::function<void(long long,long long)>(), out3),
          "re-render after the by-id trim succeeds");
    CHECK(near_to(probe(out3, (long long)(X/2 + q), (long long)(X - q)), 0.0, 0.005),
          "the newly-trimmed span of the left half is silent");

    // ---- 7. N same-source regions, each unambiguously addressable -----------
    // The punch path (own_clip + add_region_shared) schedules several regions
    // over ONE parent; the pointer API can only ever reach the first, which is
    // the root cause this harness exists to pin down.  The id API must reach
    // each one.
    const int track2 = audio_app_master_add_track(0);
    const AudioClip* parent = audio_app_project_own_clip(track2, src);
    CHECK(parent != nullptr, "own_clip stores a shared parent");
    CHECK(audio_app_project_add_region_shared(track2, parent, 0,     0, L/4, 1.f, 0, 0),
          "shared region A scheduled");
    const unsigned long long idA = audio_app_project_last_region_id(track2);
    CHECK(audio_app_project_add_region_shared(track2, parent, L/2, L/2, L/4, 1.f, 0, 0),
          "shared region B scheduled");
    const unsigned long long idB = audio_app_project_last_region_id(track2);
    CHECK(idA != 0 && idB != 0 && idA != idB, "A and B have distinct ids");
    CHECK(audio_app_project_set_region_by_id(track2, idB, L/2, L/2, L/8),
          "region B edits by id");
    long long ap=0,ao=0,al=0, bp=0,bo=0,bl=0;
    audio_app_project_find_region_by_id(track2, idA, &ap, &ao, &al);
    audio_app_project_find_region_by_id(track2, idB, &bp, &bo, &bl);
    CHECK(ap == 0 && ao == 0 && al == L/4, "region A untouched by B's edit");
    CHECK(bp == L/2 && bo == L/2 && bl == L/8, "region B carries B's edit");
    // The historical pointer-based call, aimed "at the track's clip": document
    // that it can only ever reach the FIRST region -- the reason every caller
    // that may face multiple slices must use ids.
    CHECK(audio_app_project_set_region(track2, parent, 0, 0, L/8),
          "pointer-based set_region still works for the first region");
    audio_app_project_find_region_by_id(track2, idA, &ap, &ao, &al);
    audio_app_project_find_region_by_id(track2, idB, &bp, &bo, &bl);
    CHECK(al == L/8 && bl == L/8 && bp == L/2,
          "pointer-based edit landed on the FIRST region only");

    // ---- 8. ids survive a track partition -----------------------------------
    CHECK(audio_app_project_partition_track(track2, L/16, L/12),
          "partition removes a span from region A");
    CHECK(audio_app_project_region_id_at(track2, 0) == idA,
          "A's left child keeps A's stable id across the partition");
    bool bStill = false;
    for (int i = 0; i < audio_app_project_region_count(track2); ++i)
        if (audio_app_project_region_id_at(track2, i) == idB) bStill = true;
    CHECK(bStill, "B (outside the cut) keeps its id across the partition");

    // ---- 9. remove ONE slice by id, the other keeps playing -----------------
    CHECK(audio_app_project_remove_region_by_id(track2, idB), "remove region B by id");
    bool bGone = true, aKept = false;
    for (int i = 0; i < audio_app_project_region_count(track2); ++i) {
        const unsigned long long id = audio_app_project_region_id_at(track2, i);
        if (id == idB) bGone = false;
        if (id == idA) aKept = true;
    }
    CHECK(bGone && aKept, "exactly the removed slice is gone; its sibling remains");


    //------------------------------------------------------------------------
    //  10. SAMPLE-EXACT slice alignment.
    //
    //  Every check above measures WHICH source material a piece plays.  This
    //  one measures WHERE, to the sample: each piece's sourceOffset has to keep
    //  its material aligned to the timeline exactly, and the cuts are made at
    //  ODD sample positions (prime offsets, never a block or period multiple)
    //  because an alignment error that only shows off the block grid is exactly
    //  the kind that survives a coarse test.  Repeated slicing (a slice of a
    //  slice), a move, and a duplicate of a slice are all covered, since a
    //  source offset that is folded or re-derived once can be folded twice.
    //------------------------------------------------------------------------
    {
        AudioClip saws;
        saws.name = "saw";
        saws.sampleRate = saws.sourceSampleRate = sr;
        saws.resize(L);
        for (long long i = 0; i < L; ++i) {
            const float v = (float)saw(i);
            saws.ch[0][(size_t)i] = v;
            saws.ch[1][(size_t)i] = v;
        }

        const int trk = audio_app_master_add_track(0);
        const AudioClip* par = audio_app_project_own_clip(trk, saws);
        CHECK(trk >= 0 && par != nullptr, "sample-exact: track + shared source");

        // whole clip, then cut at an ODD sample position
        CHECK(audio_app_project_add_region_shared(trk, par, 0, 0, L, 1.f, 0, 0),
              "sample-exact: whole clip scheduled");
        const unsigned long long idA2 = audio_app_project_last_region_id(trk);
        const long long C1 = L / 2 + 37;                  // odd, off every grid
        CHECK(audio_app_project_add_region_shared(trk, par, C1, C1, L - C1, 1.f, 0, 0),
              "sample-exact: right half of an ODD cut scheduled at {C1, C1}");
        const unsigned long long idB2 = audio_app_project_last_region_id(trk);
        CHECK(audio_app_project_set_region_by_id(trk, idA2, 0, 0, C1),
              "sample-exact: left half trimmed to the odd cut");

        // slice the slice, again at an odd position
        const long long C2 = C1 + (L - C1) / 2 + 13;
        CHECK(audio_app_project_add_region_shared(trk, par, C2, C2, L - C2, 1.f, 0, 0),
              "sample-exact: second cut (a slice of a slice) scheduled");
        const unsigned long long idC2 = audio_app_project_last_region_id(trk);
        CHECK(audio_app_project_set_region_by_id(trk, idB2, C1, C1, C2 - C1),
              "sample-exact: middle piece trims to the second cut");

        // move the middle piece to an odd position past the end, and duplicate
        // the last piece there too -- a slice pasted elsewhere keeps its own
        // source offset and must stay sample-aligned to its new start.
        const long long M  = L + 20011;                   // odd move target
        const long long PB = L + 60013;                   // odd paste target
        CHECK(audio_app_project_set_region_by_id(trk, idB2, M, C1, C2 - C1),
              "sample-exact: middle piece moved to an odd timeline position");
        CHECK(audio_app_project_add_region_shared(trk, par, PB, C2, L - C2, 1.f, 0, 0),
              "sample-exact: last piece duplicated at an odd timeline position");
        const unsigned long long idD2 = audio_app_project_last_region_id(trk);

        AudioClip o4;
        const double secs = (double)(PB + (L - C2)) / sr + 0.2;
        CHECK(audio_app_freeze_render_at(trk, 0, secs, std::function<void()>(),
                                         std::function<void(long long,long long)>(), o4),
              "sample-exact: offline render through the real playback path");

        // probe each piece well inside its own span (clear of any declick ramp)
        const long long pad = 2400;                       // 50 ms at 48 k
        struct Pz { const char* name; long long start, off, len; };
        const Pz pz[] = {
            { "left piece (cut at an odd sample)",        0,  0,  C1 },
            { "middle piece MOVED to an odd position",    M,  C1, C2 - C1 },
            { "last piece (a slice of a slice)",          C2, C2, L - C2 },
            { "DUPLICATE of the last piece, pasted odd",  PB, C2, L - C2 },
        };
        for (size_t i = 0; i < sizeof(pz) / sizeof(pz[0]); ++i) {
            double br = 0, rr = 0;
            const long long a = pz[i].start + pad;
            const long long b = pz[i].start + pz[i].len - pad;
            const long long k = best_shift(o4, pz[i].start, pz[i].off, a, b, &br, &rr);
            std::printf("      %-44s best shift %+lld  r=%.6f (next %.6f)\n",
                        pz[i].name, k, br, rr);
            char msg[200];
            std::snprintf(msg, sizeof msg,
                          "sample-exact: %s is aligned to the sample", pz[i].name);
            CHECK(k == 0 && br > 0.999 && rr < 0.99, msg);
        }
    }
    //------------------------------------------------------------------------
    //  REGION LOOPING, GAIN, MUTE.  The loop pass must repeat the region's OWN
    //  trimmed span at its own source offset -- not the whole file from zero --
    //  and every pass must land sample-exact. The saw encodes its own source
    //  index, so a wrong pass start shows up as a nonzero best shift.
    //------------------------------------------------------------------------
    {
        AudioClip lsrc;
        lsrc.name = "saw-loop";
        lsrc.sampleRate = lsrc.sourceSampleRate = sr;
        lsrc.resize(L);
        for (long long i = 0; i < L; ++i) {
            const float v = (float)saw(i);
            lsrc.ch[0][(size_t)i] = v;
            lsrc.ch[1][(size_t)i] = v;
        }
        const int trk = audio_app_master_add_track(0);
        const AudioClip* par = audio_app_project_own_clip(trk, lsrc);
        CHECK(trk >= 0 && par != nullptr, "loop: track + shared source");

        //  A region trimmed to a sub-span, looped to fill four passes.  Both
        //  the offset and the period are odd and coprime with the saw period,
        //  so a fold that used the wrong base cannot coincidentally line up.
        const long long OFF = 5003;                  // odd source offset
        const long long PER = 4001;                  // odd loop period
        const long long REG = 4 * PER;               // four passes
        CHECK(audio_app_project_add_region_shared(trk, par, 0, OFF, REG, 1.f, 0, 0),
              "loop: looped region scheduled");
        const unsigned long long lid = audio_app_project_last_region_id(trk);
        CHECK(audio_app_project_set_loop_by_id(trk, lid, true), "loop: loop flag set");
        CHECK(audio_app_project_set_loop_length_by_id(trk, lid, PER),
              "loop: loop period set to the region's own trim");

        AudioClip lo;
        CHECK(audio_app_freeze_render_at(trk, 0, (double)REG / sr + 0.2,
                                         std::function<void()>(),
                                         std::function<void(long long,long long)>(), lo),
              "loop: offline render of the looped region succeeds");

        //  Every pass must restart at OFF.  Probe inside each pass, clear of
        //  the wrap declick (an eighth of a period in from each edge).
        bool allPassesExact = true;
        long long worstShift = 0; double worstR = 2.0;
        for (int k = 0; k < 4; ++k) {
            const long long a = k * PER + PER / 8;
            const long long b = k * PER + PER - PER / 8;
            double br = 0, rr = 0;
            //  A pass beginning at timeline k*PER plays source OFF onward.
            const long long sh = best_shift(lo, k * PER, OFF, a, b, &br, &rr);
            if (sh != 0 || br < 0.999) { allPassesExact = false; }
            if (br < worstR) { worstR = br; worstShift = sh; }
            std::printf("      pass %d: best shift %lld  r=%.6f  (next %.4f)\n",
                        k, sh, br, rr);
        }
        CHECK(allPassesExact,
              "loop: EVERY pass restarts at the region's own source offset, sample-exact");
        std::printf("      worst pass: shift %lld r=%.6f\n", worstShift, worstR);

        //  GAIN is applied exactly and does not disturb alignment.
        CHECK(audio_app_project_set_gain_by_id(trk, lid, 0.5f), "loop: gain set to 0.5");
        AudioClip lg;
        CHECK(audio_app_freeze_render_at(trk, 0, (double)REG / sr + 0.2,
                                         std::function<void()>(),
                                         std::function<void(long long,long long)>(), lg),
              "loop: re-render at half gain succeeds");
        {
            const long long a = PER + PER / 8, b = PER + PER - PER / 8;
            const double full = probe(lo, a, b), half = probe(lg, a, b);
            std::printf("      gain probe: full=%.4f half=%.4f ratio=%.4f\n",
                        full, half, full > 0 ? half / full : 0.0);
            CHECK(full > 0.01 && near_to(half / full, 0.5, 0.02),
                  "loop: gain 0.5 halves the rendered level exactly");
            double br = 0, rr = 0;
            const long long sh = best_shift(lg, PER, OFF, a, b, &br, &rr);
            CHECK(sh == 0 && br > 0.999, "loop: gain does not disturb sample alignment");
        }

        //  MUTE silences the region completely.
        CHECK(audio_app_project_set_gain_by_id(trk, lid, 1.f), "loop: gain restored");
        CHECK(audio_app_project_set_muted_by_id(trk, lid, true), "loop: region muted");
        AudioClip lm;
        CHECK(audio_app_freeze_render_at(trk, 0, (double)REG / sr + 0.2,
                                         std::function<void()>(),
                                         std::function<void(long long,long long)>(), lm),
              "loop: re-render while muted succeeds");
        CHECK(near_to(probe(lm, PER + PER / 8, PER + PER - PER / 8), 0.0, 0.002),
              "loop: a muted looped region is silent");

        //  UNMUTING restores it, still sample-exact (mute must not consume a pass).
        CHECK(audio_app_project_set_muted_by_id(trk, lid, false), "loop: region unmuted");
        AudioClip lu;
        CHECK(audio_app_freeze_render_at(trk, 0, (double)REG / sr + 0.2,
                                         std::function<void()>(),
                                         std::function<void(long long,long long)>(), lu),
              "loop: re-render after unmute succeeds");
        {
            double br = 0, rr = 0;
            const long long a = 2 * PER + PER / 8, b = 2 * PER + PER - PER / 8;
            const long long sh = best_shift(lu, 2 * PER, OFF, a, b, &br, &rr);
            std::printf("      after unmute: shift %lld r=%.6f\n", sh, br);
            CHECK(sh == 0 && br > 0.999,
                  "loop: unmuting returns the same sample-exact pass alignment");
        }
    }
    //------------------------------------------------------------------------
    //  DEGENERATE INPUTS.  Nothing here should be reachable from a sane UI
    //  gesture, but all of it is reachable from a corrupt/older project file
    //  or an off-by-one in a caller -- and a loop whose pass length computes
    //  to zero is the classic way to hang an audio thread forever.
    //------------------------------------------------------------------------
    {
        AudioClip dsrc;
        dsrc.name = "saw-degenerate";
        dsrc.sampleRate = dsrc.sourceSampleRate = sr;
        dsrc.resize(L);
        for (long long i = 0; i < L; ++i) {
            const float v = (float)saw(i);
            dsrc.ch[0][(size_t)i] = v;
            dsrc.ch[1][(size_t)i] = v;
        }
        const int trk = audio_app_master_add_track(0);
        const AudioClip* par = audio_app_project_own_clip(trk, dsrc);
        CHECK(trk >= 0 && par != nullptr, "degenerate: track + shared source");

        //  (a) loop period LONGER than the audio available after the offset.
        const long long OFF = L - 5000;
        CHECK(audio_app_project_add_region_shared(trk, par, 0, OFF, 40000, 1.f, 0, 0),
              "degenerate: region near the end of the source scheduled");
        const unsigned long long d1 = audio_app_project_last_region_id(trk);
        CHECK(audio_app_project_set_loop_by_id(trk, d1, true), "degenerate: loop on");
        CHECK(audio_app_project_set_loop_length_by_id(trk, d1, 999999),
              "degenerate: an over-long loop period is accepted");
        AudioClip d1out;
        CHECK(audio_app_freeze_render_at(trk, 0, 40000.0 / sr + 0.2,
                                         std::function<void()>(),
                                         std::function<void(long long,long long)>(), d1out),
              "degenerate: an over-long loop period renders (no hang, no crash)");

        //  (b) source offset PAST the end of the audio, looping on: the pass
        //      length computes to zero. This must be silence, not a spin.
        CHECK(audio_app_project_set_region_by_id(trk, d1, 0, L + 12345, 20000),
              "degenerate: offset past the end of the source is accepted");
        AudioClip d2out;
        CHECK(audio_app_freeze_render_at(trk, 0, 20000.0 / sr + 0.2,
                                         std::function<void()>(),
                                         std::function<void(long long,long long)>(), d2out),
              "degenerate: offset past the source renders (no hang, no crash)");
        CHECK(near_to(probe(d2out, 2000, 18000), 0.0, 0.002),
              "degenerate: a region whose source offset is past the end is SILENT");

        //  (c) a one-sample loop period.
        CHECK(audio_app_project_set_region_by_id(trk, d1, 0, 1000, 20000),
              "degenerate: region restored inside the source");
        CHECK(audio_app_project_set_loop_length_by_id(trk, d1, 1),
              "degenerate: a one-sample loop period is accepted");
        AudioClip d3out;
        CHECK(audio_app_freeze_render_at(trk, 0, 20000.0 / sr + 0.2,
                                         std::function<void()>(),
                                         std::function<void(long long,long long)>(), d3out),
              "degenerate: a one-sample loop period renders (no hang, no crash)");

        //  (d) fades that together exceed the region length.
        CHECK(audio_app_project_set_loop_by_id(trk, d1, false), "degenerate: loop off");
        CHECK(audio_app_project_set_region_by_id(trk, d1, 0, 0, 8000),
              "degenerate: short region for the fade test");
        CHECK(audio_app_project_set_fades_by_id(trk, d1, 7000, 7000, 0.f, 0.f),
              "degenerate: overlapping fades are accepted");
        AudioClip d4out;
        CHECK(audio_app_freeze_render_at(trk, 0, 8000.0 / sr + 0.2,
                                         std::function<void()>(),
                                         std::function<void(long long,long long)>(), d4out),
              "degenerate: fades longer than the region render (no hang, no crash)");
        {
            //  Whatever the overlap policy is, the result must stay bounded --
            //  a fade pair that multiplies past unity would clip the mix.
            double peak = 0.0;
            const long long n = (long long)d4out.ch[0].size() < 8000
                              ? (long long)d4out.ch[0].size() : 8000;
            for (long long i = 0; i < n; ++i) {
                const double a = std::fabs((double)d4out.ch[0][(size_t)i]);
                if (a > peak) peak = a;
            }
            std::printf("      overlapping-fade peak: %.4f\n", peak);
            CHECK(peak <= 1.0, "degenerate: overlapping fades never exceed full scale");
        }

        //  (e) a zero-length region.
        CHECK(audio_app_project_set_fades_by_id(trk, d1, 0, 0, 0.f, 0.f),
              "degenerate: fades cleared");
        CHECK(audio_app_project_set_region_by_id(trk, d1, 0, 0, 0),
              "degenerate: a zero-length region is accepted");
        AudioClip d5out;
        CHECK(audio_app_freeze_render_at(trk, 0, 4000.0 / sr + 0.2,
                                         std::function<void()>(),
                                         std::function<void(long long,long long)>(), d5out),
              "degenerate: a zero-length region renders (no hang, no crash)");
    }
    //------------------------------------------------------------------------
    //  CONSOLIDATE.  Now that the mixer is engine code (consolidate.h) instead
    //  of a lambda in main.cpp, the part that decides what you HEAR can finally
    //  be asserted on samples: gap placement, gain, fade curves, loop wrap.
    //------------------------------------------------------------------------
    {
        AudioClip src;
        src.name = "saw-consolidate";
        src.sampleRate = src.sourceSampleRate = sr;
        src.resize(L);
        for (long long i = 0; i < L; ++i) {
            const float v = (float)saw(i);
            src.ch[0][(size_t)i] = v;
            src.ch[1][(size_t)i] = v;
        }

        //  Two pieces with a GAP between them -- exactly what cutting a clip
        //  and moving the halves apart leaves behind.
        const long long P0 = 0,     LEN0 = 30000;
        const long long GAP = 12000;
        const long long P1 = LEN0 + GAP, LEN1 = 25000;
        const long long OFF1 = 44444;            // the right piece's own offset
        std::vector<ConsolidatePiece> pcs;
        {
            ConsolidatePiece a; a.clip = &src; a.pos = P0; a.off = 0;    a.len = LEN0;
            ConsolidatePiece b; b.clip = &src; b.pos = P1; b.off = OFF1; b.len = LEN1;
            pcs.push_back(a); pcs.push_back(b);
        }
        const long long SPAN_A = 0, SPAN_B = P1 + LEN1;
        AudioClip cons = consolidateRender(pcs, SPAN_A, SPAN_B, sr);
        CHECK((long long)cons.numFrames() == SPAN_B - SPAN_A,
              "consolidate: the output spans the whole selection, blank space included");

        //  Each piece must land sample-exact at its own timeline position,
        //  reading from its own source offset.
        {
            double br = 0, rr = 0;
            const long long sh = best_shift(cons, P0 - SPAN_A, 0,
                                            2000, LEN0 - 2000, &br, &rr);
            std::printf("      consolidate piece 0: shift %lld r=%.6f\n", sh, br);
            CHECK(sh == 0 && br > 0.999, "consolidate: piece 0 is sample-exact");
        }
        {
            double br = 0, rr = 0;
            const long long sh = best_shift(cons, P1 - SPAN_A, OFF1,
                                            P1 + 2000, P1 + LEN1 - 2000, &br, &rr);
            std::printf("      consolidate piece 1: shift %lld r=%.6f\n", sh, br);
            CHECK(sh == 0 && br > 0.999,
                  "consolidate: piece 1 keeps its OWN source offset, sample-exact");
        }
        //  The gap is the point of the manual's wording: it must be silence,
        //  not the two pieces butted together.
        CHECK(near_to(probe(cons, LEN0 + 500, P1 - 500), 0.0, 0.0005),
              "consolidate: the gap between pieces is SILENT, not closed up");

        //  GAIN is baked in.
        {
            std::vector<ConsolidatePiece> g = pcs;
            g[0].gain = 0.25f;
            AudioClip cg = consolidateRender(g, SPAN_A, SPAN_B, sr);
            const double full = probe(cons, 2000, LEN0 - 2000);
            const double quiet = probe(cg,  2000, LEN0 - 2000);
            std::printf("      consolidate gain: full=%.4f quarter=%.4f ratio=%.4f\n",
                        full, quiet, full > 0 ? quiet / full : 0.0);
            CHECK(full > 0.01 && near_to(quiet / full, 0.25, 0.005),
                  "consolidate: a piece's gain is baked into the render exactly");
        }

        //  MUTE contributes silence, and does not shift anything else.
        {
            std::vector<ConsolidatePiece> mset = pcs;
            mset[0].muted = true;
            AudioClip cm = consolidateRender(mset, SPAN_A, SPAN_B, sr);
            CHECK(near_to(probe(cm, 2000, LEN0 - 2000), 0.0, 0.0005),
                  "consolidate: a muted piece renders as silence");
            double br = 0, rr = 0;
            const long long sh = best_shift(cm, P1 - SPAN_A, OFF1,
                                            P1 + 2000, P1 + LEN1 - 2000, &br, &rr);
            CHECK(sh == 0 && br > 0.999,
                  "consolidate: muting one piece does not move the others");
        }

        //  FADES use the SAME curve playback uses.  A fade-in must start near
        //  silence and reach full level, and must match ScheduledClip::fadeGain
        //  sample for sample -- that equality is the whole claim.
        {
            std::vector<ConsolidatePiece> f = pcs;
            f[0].fade.fadeInFrames = 8000;
            f[0].hasFade = true;
            AudioClip cf = consolidateRender(f, SPAN_A, SPAN_B, sr);
            const double early = probe(cf, 100, 400);
            const double late  = probe(cf, 12000, 20000);
            std::printf("      consolidate fade: early=%.4f late=%.4f\n", early, late);
            CHECK(early < late * 0.35, "consolidate: a fade-in actually fades in");

            bool exact = true;
            double worst = 0.0;
            for (long long k = 200; k < 8000; k += 137) {
                const float g = f[0].fade.fadeGain(k, LEN0);
                const double want = (double)src.ch[0][(size_t)k] * (double)g;
                const double got  = (double)cf.ch[0][(size_t)k];
                const double d = std::fabs(want - got);
                if (d > worst) worst = d;
                if (d > 1e-5) exact = false;
            }
            std::printf("      consolidate fade vs fadeGain: worst delta %.3e\n", worst);
            CHECK(exact,
                  "consolidate: the baked fade equals ScheduledClip::fadeGain sample for sample");
        }

        //  A LOOPING piece wraps its source, and still lands sample-exact.
        {
            std::vector<ConsolidatePiece> lp;
            ConsolidatePiece a; a.clip = &src; a.pos = 0;
            a.off = L - 6000;                 // near the end, so it must wrap
            a.len = 20000; a.loop = true;
            lp.push_back(a);
            AudioClip cl = consolidateRender(lp, 0, 20000, sr);
            CHECK((long long)cl.numFrames() == 20000, "consolidate: looped piece renders");
            double br = 0, rr = 0;
            const long long sh = best_shift(cl, 0, L - 6000, 500, 5500, &br, &rr);
            CHECK(sh == 0 && br > 0.999,
                  "consolidate: a looping piece is sample-exact before the wrap");
            //  After the wrap the source restarts at frame 0.
            const long long w = 6000;
            const double d0 = std::fabs((double)cl.ch[0][(size_t)(w + 10)] - saw(10));
            std::printf("      consolidate loop wrap delta: %.3e\n", d0);
            CHECK(d0 < 1e-6, "consolidate: the wrap restarts the source at frame 0");
        }

        //  Degenerate: an empty selection and a reversed span must not crash.
        {
            std::vector<ConsolidatePiece> none;
            AudioClip e0 = consolidateRender(none, 0, 1000, sr);
            CHECK((long long)e0.numFrames() == 1000 &&
                  near_to(probe(e0, 10, 990), 0.0, 1e-6),
                  "consolidate: an empty selection yields silence of the right length");
            AudioClip e1 = consolidateRender(pcs, 500, 100, sr);
            CHECK(e1.numFrames() == 0, "consolidate: a reversed span yields nothing");
        }
    }




    //------------------------------------------------------------------------
    //  TRANSPORT LOOP WRAP.  The engine loop is a BACKWARD jump of the
    //  transport at the exact loop-end frame (audio_render_device cycle-splits
    //  there).  Everything above renders a monotonic timeline; this section
    //  renders through MANY wraps and asserts on every single pass, because
    //  the reported failure ("silence for a few loops, then it comes back --
    //  sometimes it stays silent") is intermittent per pass and invisible to
    //  any aggregate probe.
    //
    //  The real device callback is pumped directly (AudioEngine::render_into
    //  -> audio_render_device -> cycle split -> graph render), with the stream
    //  stopped, so this is the exact realtime path.  Which timeline sample
    //  each rendered frame belongs to is reconstructed by replaying the wrap
    //  rule: pos+1, wrapping to loopStart at loopEnd.
    //------------------------------------------------------------------------
    {
        PatchKnob::engine::AudioEngine* eng = audio_app_engine();
        const int block = eng ? (int)eng->blockSize() : 0;
        const int devCh = eng ? (int)eng->numChannels() : 0;
        CHECK(eng != nullptr && block > 0 && devCh >= 1,
              "wrap: engine + granted block size");

        // Engine loop of ~0.5 s at ~20 s.  That window sits far past every
        // region the sections above scheduled (their material all ends before
        // ~4 s), so nothing but this section's own regions is audible in it --
        // and it sits strictly INSIDE the regions scheduled below, so a wrap
        // is a pure backward transport jump mid-region, no region edge
        // involved.  Ticks derive from samples through the app's own clock so
        // the harness carries no ppqn/tempo assumption of its own; the exact
        // granted loop bounds are read back for the wrap replay below.
        audio_app_set_loop_ticks(audio_app_sample_to_tick((long long)(20.0 * sr)),
                                 audio_app_sample_to_tick((long long)(20.5 * sr)), 1);
        long long ls = 0, le = 0; int lon = 0;
        audio_app_loop_debug(&ls, &le, nullptr, nullptr, &lon);
        std::printf("      loop window [%lld, %lld) span %lld, block %d\n",
                    ls, le, le - ls, block);
        const long long regStart = (long long)sr * 18;   // region starts BEFORE the loop
        const long long WL = (long long)sr * 5;          // 5 s source (18 s .. 23 s)
        CHECK(lon != 0 && ls > regStart + (long long)sr / 2 &&
              le > ls && le + (long long)sr / 2 < regStart + WL,
              "wrap: loop window is inside the scheduled regions");

        static const int kWraps = 24;                // >= 20, checked PER WRAP
        const long long pad = 2400;                  // 50 ms clear of the splice

        // Device-channel scratch for render_into (paNonInterleaved layout).
        std::vector<std::vector<float>> devBuf((size_t)devCh);
        std::vector<float*> devPtr((size_t)devCh);
        for (int c = 0; c < devCh; ++c) {
            devBuf[(size_t)c].assign((size_t)block, 0.f);
            devPtr[(size_t)c] = devBuf[(size_t)c].data();
        }

        // Pearson correlation of per-wrap audio in [a,b) against an arbitrary
        // expected-source function of the timeline sample (the plain corr_at
        // cannot express a region-looped expectation).
        struct WrapStats { double amp, r, rival; long long shift; };
        AudioClip per; per.resize(le);               // timeline-indexed, reused
        typedef std::function<double(long long)> Expect;
        auto corrFnAt = [&](const Expect& exp0,
                            long long a, long long b, long long shift) -> double {
            const double n = (double)(b - a);
            double sx = 0, sy = 0;
            for (long long s = a; s < b; ++s) {
                sx += per.ch[0][(size_t)s];
                sy += exp0(s + shift);
            }
            const double mx = sx / n, my = sy / n;
            double sxy = 0, sxx = 0, syy = 0;
            for (long long s = a; s < b; ++s) {
                const double dx = per.ch[0][(size_t)s] - mx;
                const double dy = exp0(s + shift) - my;
                sxy += dx * dy; sxx += dx * dx; syy += dy * dy;
            }
            return (sxx > 0 && syy > 0) ? sxy / std::sqrt(sxx * syy) : -2.0;
        };

        // Render `kWraps` passes of the loop window through the device path and
        // measure every pass: mean |amplitude| and the best alignment shift.
        auto runWraps = [&](const Expect& exp0, WrapStats* st) {
            audio_app_transport_locate(ls);
            audio_app_patch_set_playing(true);
            long long pos = ls;
            int w = 0;
            while (w < kWraps) {
                eng->render_into(nullptr, (void*)devPtr.data(),
                                 (unsigned long)block, 0);
                for (int i = 0; i < block && w < kWraps; ++i) {
                    per.ch[0][(size_t)pos] = devBuf[0][(size_t)i];
                    if (++pos >= le) {
                        WrapStats& o = st[w];
                        o.amp = probe(per, ls + pad, le - pad);
                        o.shift = 0; o.r = -2.0; o.rival = -2.0;
                        for (long long k = -8; k <= 8; ++k) {
                            const double r = corrFnAt(exp0,
                                                      ls + pad, le - pad, k);
                            if (r > o.r) { o.rival = o.r; o.r = r; o.shift = k; }
                            else if (r > o.rival) o.rival = r;
                        }
                        pos = ls; ++w;
                    }
                }
            }
            audio_app_patch_set_playing(false);
            audio_app_transport_locate_sync(0);
        };

        auto report = [&](const char* what, const WrapStats* st,
                          double minR, double& worstAmp, int& bad) {
            worstAmp = 1e9; bad = 0;
            double refAmp = 0;
            for (int w = 0; w < kWraps; ++w) refAmp = std::max(refAmp, st[w].amp);
            for (int w = 0; w < kWraps; ++w) {
                const bool aok = st[w].amp > 0.05 && st[w].amp > refAmp * 0.9;
                const bool sok = st[w].shift == 0 && st[w].r > minR;
                if (st[w].amp < worstAmp) worstAmp = st[w].amp;
                if (!aok || !sok) {
                    ++bad;
                    std::printf("      %s wrap %2d BAD: amp %.4f shift %+lld r=%.6f (next %.4f)\n",
                                what, w, st[w].amp, st[w].shift, st[w].r, st[w].rival);
                }
            }
            std::printf("      %s: %d wraps, worst amp %.4f, bad %d\n",
                        what, kWraps, worstAmp, bad);
        };

        // ---- (a) plain region spanning the loop window ---------------------
        {
            AudioClip wsrc;
            wsrc.name = "saw-transport-loop";
            wsrc.sampleRate = wsrc.sourceSampleRate = sr;
            wsrc.resize(WL);
            for (long long i = 0; i < WL; ++i) {
                const float v = (float)saw(i);
                wsrc.ch[0][(size_t)i] = v;
                wsrc.ch[1][(size_t)i] = v;
            }
            const int trk = audio_app_master_add_track(0);
            const AudioClip* par = audio_app_project_own_clip(trk, wsrc);
            CHECK(trk >= 0 && par != nullptr, "wrap: plain-region track + source");
            CHECK(audio_app_project_add_region_shared(trk, par, regStart, 0, WL,
                                                      1.f, 0, 0),
                  "wrap: plain region scheduled across the loop window");

            static WrapStats st[kWraps];
            // Region {start regStart, off 0}: timeline s plays saw(s - regStart).
            runWraps([&](long long s) { return saw(s - regStart); }, st);
            double worstAmp; int bad;
            report("plain", st, 0.999, worstAmp, bad);
            CHECK(bad == 0,
                  "wrap: EVERY pass of a plain region is audible and sample-exact");

            // done with this track's region; silence it for the next variants
            CHECK(audio_app_project_set_muted_by_id(
                      trk, audio_app_project_last_region_id(trk), true),
                  "wrap: plain region muted for the next variant");
        }

        // ---- (b) region-LOOPED region under the transport loop -------------
        // Two nested loops: the region wraps its own trimmed source span while
        // the transport wraps the timeline.
        {
            AudioClip lsrc;
            lsrc.name = "saw-region-loop";
            lsrc.sampleRate = lsrc.sourceSampleRate = sr;
            lsrc.resize(WL);
            for (long long i = 0; i < WL; ++i) {
                const float v = (float)saw(i);
                lsrc.ch[0][(size_t)i] = v;
                lsrc.ch[1][(size_t)i] = v;
            }
            const int trk = audio_app_master_add_track(0);
            const AudioClip* par = audio_app_project_own_clip(trk, lsrc);
            CHECK(trk >= 0 && par != nullptr, "wrap: looped-region track + source");
            const long long kOff = 5003;             // odd source offset
            const long long kPer = 12007;            // odd region-loop period
            CHECK(audio_app_project_add_region_shared(trk, par, regStart, kOff, WL,
                                                      1.f, 0, 0),
                  "wrap: looped region scheduled");
            const unsigned long long lid = audio_app_project_last_region_id(trk);
            CHECK(audio_app_project_set_loop_by_id(trk, lid, true) &&
                  audio_app_project_set_loop_length_by_id(trk, lid, kPer),
                  "wrap: region loop period set");

            static WrapStats st[kWraps];
            // Region {start regStart, off kOff, loop period kPer}: timeline s
            // plays source kOff + ((s - regStart) mod kPer).
            runWraps([&](long long s) {
                        long long m = (s - regStart) % kPer; if (m < 0) m += kPer;
                        return saw(kOff + m);
                     }, st);
            double worstAmp; int bad;
            // The region-loop splice declick (~2 ms crossfade every kPer)
            // legitimately reshapes a sliver of each pass: accept r > 0.99.
            report("region-loop", st, 0.99, worstAmp, bad);
            CHECK(bad == 0,
                  "wrap: EVERY pass of a region-looped region is audible and exact");
            CHECK(audio_app_project_set_muted_by_id(trk, lid, true),
                  "wrap: looped region muted for the next variant");
        }

        // ---- (c) realtime-WARPED region under the transport loop -----------
        // The signalsmith stretcher carries internal history; a backward jump
        // must reset + re-prime it.  Probe each pass in three sub-windows so a
        // dropout confined to the start of a pass (an unprimed stretcher) is
        // seen even when the whole-pass average looks alive.
        {
            AudioClip wsin = AudioClip::synth_sine(330.0, 4.0, sr, 0.8f, "warp-sine");
            const int trk = audio_app_master_add_track(0);
            const AudioClip* par = audio_app_project_own_clip(trk, wsin);
            CHECK(trk >= 0 && par != nullptr, "wrap: warped track + source");
            const int fz = audio_app_freeze_attach_shared(trk, par, regStart, 1.f);
            CHECK(fz >= 0, "wrap: warped region attached");
            // 4 s of source over 5 s of timeline (a 0.8x stretch), covering
            // 18 s .. 23 s -- the loop window sits in its middle.
            PatchKnob::engine::WarpMarker wm[2];
            wm[0].srcSample = 0;                  wm[0].dstSample = 0;
            wm[1].srcSample = (long long)sr * 4;  wm[1].dstSample = (long long)sr * 5;
            CHECK(audio_app_freeze_set_warp(fz, wm, 2), "wrap: warp map published");

            audio_app_transport_locate(ls);
            audio_app_patch_set_playing(true);
            const long long span = le - ls, q = span / 4;
            double onset[kWraps], head[kWraps], mid[kWraps], tail[kWraps];
            long long pos = ls;
            int w = 0;
            while (w < kWraps) {
                eng->render_into(nullptr, (void*)devPtr.data(),
                                 (unsigned long)block, 0);
                for (int i = 0; i < block && w < kWraps; ++i) {
                    per.ch[0][(size_t)pos] = devBuf[0][(size_t)i];
                    if (++pos >= le) {
                        onset[w] = probe(per, ls,          ls + 1000);
                        head[w]  = probe(per, ls,          ls + q);
                        mid[w]   = probe(per, ls + q,      le - q);
                        tail[w]  = probe(per, le - q,      le);
                        pos = ls; ++w;
                    }
                }
            }
            audio_app_patch_set_playing(false);
            audio_app_transport_locate_sync(0);

            double refM = 0;
            for (int k = 0; k < kWraps; ++k) refM = std::max(refM, mid[k]);
            int bad = 0;
            for (int k = 0; k < kWraps; ++k) {
                // Whole-pass presence in every third of the window, AND a live
                // ONSET: before the outputSeek restart fix the stretcher owed
                // its whole output latency after every wrap, so each pass began
                // with ~45 ms of silence -- onset[] measured 0.0000 on every
                // single wrap while the pass averages still looked healthy.
                const bool ok = onset[k] > refM * 0.4 &&
                                head[k]  > 0.05 && head[k] > refM * 0.5 &&
                                mid[k]   > 0.05 && mid[k]  > refM * 0.5 &&
                                tail[k]  > 0.05 && tail[k] > refM * 0.5;
                if (!ok) {
                    ++bad;
                    std::printf("      warp wrap %2d BAD: onset %.4f head %.4f mid %.4f tail %.4f\n",
                                k, onset[k], head[k], mid[k], tail[k]);
                }
            }
            std::printf("      warp: %d wraps, onset[0]=%.4f head[0]=%.4f mid[0]=%.4f tail[0]=%.4f, bad %d\n",
                        kWraps, onset[0], head[0], mid[0], tail[0], bad);
            CHECK(bad == 0,
                  "wrap: EVERY pass of a realtime-warped region is audible, "
                  "including the first millisecond after the backward jump");
            audio_app_freeze_detach(fz);
        }

        audio_app_set_loop_ticks(0, 0, 0);           // leave the loop off
    }

    std::printf(g_fail ? "SELFTEST: %d FAILED\n" : "SELFTEST: all passed\n", g_fail);
    audio_app_shutdown();
    return g_fail ? 1 : 0;
}
