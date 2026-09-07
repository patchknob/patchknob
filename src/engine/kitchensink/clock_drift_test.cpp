//----------------------------------------------------------------------------
//  clock_drift_test.cpp -- 24-track sub-clock drift measurement.
//
//  Answers one question with numbers instead of argument: do independently
//  derived sub-clocks drift against each other, or against the master?
//
//  METHOD.  24 "tracks", each clicking on its own subdivision of the bar, all
//  derived from ONE TempoMap exactly the way the engine derives them
//  (TempoMap::tick_to_sample -- the production path, not a reimplementation).
//  Track i uses a 256th-note grid decimated by rate[i], so the tracks run at
//  wildly different rates but share musical coincidence points.
//
//  At c_ppqn 768: quarter = 768 ticks, so a 256th note = 768/64 = 12 ticks
//  EXACTLY -- no rounding anywhere in the grid itself.
//
//  Three measurements:
//    1. COINCIDENCE   at every musical position where two tracks land on the
//                     same tick, their derived sample positions must be bit
//                     identical.  Any nonzero delta is inter-clock drift.
//    2. ABSOLUTE      each event's derived sample vs. the ideal rational
//                     position over a long run -- catches cumulative drift
//                     against the master that a pairwise test would miss.
//    3. HARMONIC      render coincident clicks and check they sum coherently.
//                     Misaligned impulses comb-filter; aligned ones do not.
//                     Reported as the summed peak vs. the ideal N*amplitude.
//
//  Build (standalone, no engine deps beyond kitchensink):
//    g++ -std=c++17 -O2 clock_drift_test.cpp kitchensink.cpp -o clock_drift_test
//----------------------------------------------------------------------------
#include "kitchensink.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>
#include <map>

using namespace kitchensink;

namespace {

constexpr int   kTracks     = 24;
constexpr int   kSampleRate = 48000;
constexpr double kBpm       = 128.0;

// A 256th note in sequencer ticks.  Exact at seq_ppqn 768 (768/64 == 12).
constexpr int64_t k256th = seq_ppqn / 64;

static_assert(seq_ppqn % 64 == 0,
              "a 256th note must be a whole number of ticks for this test to "
              "measure the CLOCK rather than the grid's own rounding");

// Per-track rate: track i fires every rate[i]-th 256th note.  Deliberately a
// mix of powers of two and odd/prime factors -- coincidences between coprime
// rates are rare and far apart, which is where drift would show up first.
const int kRate[kTracks] = {
    1,  2,  3,  4,  5,  6,  7,  8,
    9, 12, 16, 24, 32, 48, 64, 96,
    5, 7, 11, 13, 17, 19, 23, 31
};

struct Track {
    int rate = 1;
    std::vector<int64_t> tick;     // musical position of each click
    std::vector<int64_t> sample;   // derived audio position of each click
};

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

} // namespace

int main() {
    std::printf("24-track sub-clock drift measurement\n");
    std::printf("  master           : audio sample clock (TempoMap, integer muldiv)\n");
    std::printf("  sample rate      : %d Hz\n", kSampleRate);
    std::printf("  tempo            : %.3f BPM (constant)\n", kBpm);
    std::printf("  sequencer PPQN   : %d\n", (int)seq_ppqn);
    std::printf("  Beats domain PPQN: %d  (ratio %d, exact)\n",
                (int)ticks_per_beat, (int)(ticks_per_beat / seq_ppqn));
    std::printf("  256th note       : %lld ticks (exact)\n\n", (long long)k256th);

    TempoMap map;
    map.set_sample_rate((double)kSampleRate);
    map.set_tempo(kBpm);
    map.set_meter(4, 4);

    // ~4 minutes of material so cumulative error has room to appear.
    const int64_t kBars      = 128;
    const int64_t ticksPerBar = (int64_t)seq_ppqn * 4;
    const int64_t endTick    = kBars * ticksPerBar;

    std::vector<Track> tr((size_t)kTracks);
    int64_t totalEvents = 0;
    for (int i = 0; i < kTracks; ++i) {
        tr[(size_t)i].rate = kRate[i];
        const int64_t step = k256th * kRate[i];
        for (int64_t t = 0; t < endTick; t += step) {
            tr[(size_t)i].tick.push_back(t);
            tr[(size_t)i].sample.push_back(map.tick_to_sample(t));
        }
        totalEvents += (int64_t)tr[(size_t)i].tick.size();
    }
    std::printf("generated %lld click events across %d tracks over %lld bars\n\n",
                (long long)totalEvents, kTracks, (long long)kBars);

    // ---------------------------------------------------------------------
    // 1. COINCIDENCE: same tick on two tracks -> must be the same sample.
    // ---------------------------------------------------------------------
    {
        std::map<int64_t, int64_t> seen;      // tick -> sample first derived for it
        int64_t comparisons = 0, mismatches = 0, worst = 0;
        for (int i = 0; i < kTracks; ++i)
            for (size_t k = 0; k < tr[(size_t)i].tick.size(); ++k) {
                const int64_t tk = tr[(size_t)i].tick[k];
                const int64_t sm = tr[(size_t)i].sample[k];
                std::map<int64_t,int64_t>::iterator it = seen.find(tk);
                if (it == seen.end()) { seen[tk] = sm; continue; }
                ++comparisons;
                const int64_t d = sm - it->second;
                if (d != 0) {
                    ++mismatches;
                    if (std::llabs(d) > std::llabs(worst)) worst = d;
                }
            }
        std::printf("coincidence: %lld cross-track comparisons, %lld mismatches, worst delta %lld samples\n",
                    (long long)comparisons, (long long)mismatches, (long long)worst);
        check(mismatches == 0,
              "coincidence: tracks sharing a tick derive the IDENTICAL sample (zero inter-clock drift)");
    }

    // ---------------------------------------------------------------------
    // 2. ABSOLUTE: derived sample vs. ideal rational position.
    //    ideal = tick * (60 * sr) / (bpm * ppqn), computed in long double so
    //    the reference is not itself quantised.
    // ---------------------------------------------------------------------
    {
        const long double spt =
            (long double)60.0 * (long double)kSampleRate /
            ((long double)kBpm * (long double)seq_ppqn);
        long double worstErr = 0.0L;
        int64_t worstTick = 0;
        for (int i = 0; i < kTracks; ++i)
            for (size_t k = 0; k < tr[(size_t)i].tick.size(); ++k) {
                const long double ideal = (long double)tr[(size_t)i].tick[k] * spt;
                const long double err =
                    (long double)tr[(size_t)i].sample[k] - ideal;
                if (fabsl(err) > fabsl(worstErr)) {
                    worstErr = err; worstTick = tr[(size_t)i].tick[k];
                }
            }
        const double lastSec = (double)map.tick_to_sample(endTick) / kSampleRate;
        std::printf("absolute   : worst error %.6f samples (%.4f us) at tick %lld, over %.1f s\n",
                    (double)worstErr, (double)worstErr / kSampleRate * 1e6,
                    (long long)worstTick, lastSec);
        // Half a sample is the best any integer sample position can do.
        check(fabsl(worstErr) <= 0.5L + 1e-9L,
              "absolute: every derived position within half a sample of ideal (no cumulative drift)");
    }

    // ---------------------------------------------------------------------
    // 3. HARMONIC: do coincident clicks sum coherently?
    //    Misaligned impulses comb-filter; perfectly aligned ones sum to N*amp
    //    with no spreading.  Render one bar of every track into a shared buffer
    //    and inspect the coincidence points.
    // ---------------------------------------------------------------------
    {
        const int64_t renderTicks = ticksPerBar;                 // one bar
        const int64_t nFrames = map.tick_to_sample(renderTicks) + 64;
        std::vector<double> mix((size_t)nFrames, 0.0);
        std::vector<int>    hits((size_t)nFrames, 0);

        for (int i = 0; i < kTracks; ++i) {
            const int64_t step = k256th * kRate[i];
            for (int64_t t = 0; t < renderTicks; t += step) {
                const int64_t s = map.tick_to_sample(t);
                if (s >= 0 && s < nFrames) { mix[(size_t)s] += 1.0; ++hits[(size_t)s]; }
            }
        }
        // Tick 0: every track fires. Perfect alignment => exactly kTracks.
        const int zeroHits = hits[0];
        std::printf("harmonic   : downbeat stack = %d of %d tracks on ONE sample (peak %.1f)\n",
                    zeroHits, kTracks, mix[0]);
        check(zeroHits == kTracks,
              "harmonic: all 24 tracks land on the SAME sample at tick 0 (coherent sum, no comb)");

        // No energy may appear on any sample adjacent to a coincidence point --
        // that smearing IS the comb filter.
        int smeared = 0;
        for (int64_t s = 1; s + 1 < nFrames; ++s)
            if (hits[(size_t)s] > 0 && (hits[(size_t)s - 1] > 0 || hits[(size_t)s + 1] > 0))
                ++smeared;
        std::printf("harmonic   : adjacent-sample smearing events = %d\n", smeared);
        check(smeared == 0,
              "harmonic: no click lands one sample off another (no comb filtering)");

        double peak = 0.0; int64_t peakAt = 0;
        for (int64_t s = 0; s < nFrames; ++s)
            if (mix[(size_t)s] > peak) { peak = mix[(size_t)s]; peakAt = s; }
        std::printf("harmonic   : peak %.1f at sample %lld (ideal %d at 0)\n",
                    peak, (long long)peakAt, kTracks);
    }

    // ---------------------------------------------------------------------
    // 4. ROUND TRIP across the whole run.
    // ---------------------------------------------------------------------
    {
        int64_t bad = 0;
        for (int i = 0; i < kTracks; ++i)
            for (size_t k = 0; k < tr[(size_t)i].tick.size(); ++k)
                if (map.sample_to_tick(tr[(size_t)i].sample[k]) != tr[(size_t)i].tick[k])
                    ++bad;
        std::printf("roundtrip  : %lld reconstruction failures of %lld events\n",
                    (long long)bad, (long long)totalEvents);
        check(bad == 0, "roundtrip: sample_to_tick(tick_to_sample(t)) == t for all 24 tracks");
    }

    std::printf("\n%s (%d failure%s)\n",
                g_failures ? "DRIFT DETECTED" : "NO DRIFT",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
