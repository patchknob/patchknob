/*
 * kitchensink_test.cpp - assertion-based sanity tests for the ported tempo math.
 *
 * Exits non-zero on any failed check. See kitchensink.h for the Ardour credit.
 */

#include "kitchensink.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>

using namespace kitchensink;

static int g_failures = 0;

static void
check (bool cond, const char* what)
{
	std::printf ("[%s] %s\n", cond ? "PASS" : "FAIL", what);
	if (!cond) {
		++g_failures;
	}
}

static bool
bbt_eq (const BBT& a, int32_t bars, int32_t beats, int32_t ticks)
{
	return a.bars == bars && a.beats == beats && a.ticks == ticks;
}

int
main ()
{
	/* ---------------------------------------------------------------- */
	/* Constant tempo: 120 BPM, 4/4, sr = 48000.                        */
	/* 120 BPM => 2 beats/sec => beat 4 == 2 s == 96000 samples.        */
	/* ---------------------------------------------------------------- */
	{
		TempoMap map;
		map.set_sample_rate (48000);
		map.set_tempo (120.0);
		map.set_meter (4, 4);

		const int64_t s4 = map.beats_to_sample (4.0);
		std::printf ("beats_to_sample(4) = %lld (expect 96000)\n", (long long) s4);
		check (s4 == 96000, "120bpm: beat 4 == sample 96000");

		const int64_t s2 = map.beats_to_sample (2.0);
		std::printf ("beats_to_sample(2) = %lld (expect 48000)\n", (long long) s2);
		check (s2 == 48000, "120bpm: beat 2 == sample 48000 (1 second)");

		const double b96000 = map.sample_to_beats (96000);
		std::printf ("sample_to_beats(96000) = %.6f (expect 4)\n", b96000);
		check (std::fabs (b96000 - 4.0) < 1e-9, "120bpm: sample 96000 == beat 4");

		const double b48000 = map.sample_to_beats (48000);
		check (std::fabs (b48000 - 2.0) < 1e-9, "120bpm: sample 48000 == beat 2");

		const BBT bbt0 = map.sample_to_bbt (0);
		std::printf ("sample_to_bbt(0) = %d|%d|%d (expect 1|1|0)\n", bbt0.bars, bbt0.beats, bbt0.ticks);
		check (bbt_eq (bbt0, 1, 1, 0), "sample_to_bbt(0) == 1|1|0");

		const BBT bbt96000 = map.sample_to_bbt (96000);
		std::printf ("sample_to_bbt(96000) = %d|%d|%d (expect 2|1|0)\n",
		             bbt96000.bars, bbt96000.beats, bbt96000.ticks);
		check (bbt_eq (bbt96000, 2, 1, 0), "sample_to_bbt(96000) == bar 2 beat 1 (4 beats)");

		/* beats <-> bbt round trips */
		check (bbt_eq (map.beats_to_bbt (0.0), 1, 1, 0), "beats_to_bbt(0) == 1|1|0");
		check (bbt_eq (map.beats_to_bbt (4.0), 2, 1, 0), "beats_to_bbt(4) == 2|1|0");
		check (bbt_eq (map.beats_to_bbt (5.0), 2, 2, 0), "beats_to_bbt(5) == 2|2|0");
		check (std::fabs (map.bbt_to_beats (BBT {2, 1, 0}) - 4.0) < 1e-9, "bbt_to_beats(2|1|0) == 4");
		check (std::fabs (map.bbt_to_beats (BBT {1, 3, 0}) - 2.0) < 1e-9, "bbt_to_beats(1|3|0) == 2");

		/* half beat -> PPQN/2 ticks, in the Beats domain's OWN unit (not a
		   literal: ticks_per_beat is what defines this, and it has changed) */
		const int32_t halfTicks = ticks_per_beat / 2;
		const BBT halfbeat = map.beats_to_bbt (0.5);
		std::printf ("beats_to_bbt(0.5) = %d|%d|%d (expect 1|1|%d)\n",
		             halfbeat.bars, halfbeat.beats, halfbeat.ticks, (int) halfTicks);
		check (bbt_eq (halfbeat, 1, 1, halfTicks), "beats_to_bbt(0.5) == 1|1|ticks_per_beat/2");

		const double t = map.tempo_at_sample (96000);
		std::printf ("tempo_at_sample(96000) = %.6f (expect 120)\n", t);
		check (std::fabs (t - 120.0) < 1e-9, "constant tempo_at_sample == 120");
		check (std::fabs (map.tempo_at_sample (0) - 120.0) < 1e-9, "constant tempo_at_sample(0) == 120");
	}

	/* ---------------------------------------------------------------- */
	/* A different meter: 6/8 (a bar is 3 quarter notes).               */
	/* ---------------------------------------------------------------- */
	{
		TempoMap map;
		map.set_sample_rate (48000);
		map.set_tempo (120.0);
		map.set_meter (6, 8);

		/* 6/8: one bar = 6 eighth notes = 3 quarter notes.
		 * beat 3.0 (quarters) => bar 2, beat 1. */
		const BBT b = map.beats_to_bbt (3.0);
		std::printf ("6/8 beats_to_bbt(3) = %d|%d|%d (expect 2|1|0)\n", b.bars, b.beats, b.ticks);
		check (bbt_eq (b, 2, 1, 0), "6/8: 3 quarters == bar 2");
		/* one eighth = 0.5 quarter => beat 1|2|0 */
		const BBT b2 = map.beats_to_bbt (0.5);
		check (bbt_eq (b2, 1, 2, 0), "6/8: 0.5 quarter == 1|2|0 (second eighth)");
	}

	/* ---------------------------------------------------------------- */
	/* Meter change: 4/4 for bars 1-2, then 3/4 from bar 3.             */
	/* ---------------------------------------------------------------- */
	{
		TempoMap map;
		map.set_sample_rate (48000);
		map.set_tempo (120.0);
		map.set_meter (4, 4);
		map.add_meter (3, 3, 4); /* switch to 3/4 at bar 3 */

		/* bars 1,2 are 4/4 => 8 quarters. bar 3 begins at beat 8.0 */
		check (std::fabs (map.bbt_to_beats (BBT {3, 1, 0}) - 8.0) < 1e-9, "meter change: bar 3 == beat 8");
		check (bbt_eq (map.beats_to_bbt (8.0), 3, 1, 0), "meter change: beat 8 == bar 3");
		/* bar 3 in 3/4: beat 11.0 == bar 4 beat 1 (3 quarters later) */
		check (bbt_eq (map.beats_to_bbt (11.0), 4, 1, 0), "meter change: beat 11 == bar 4 (3/4)");
	}

	/* ---------------------------------------------------------------- */
	/* RAMP: tempo ramps 120 -> 240 over beats [0, 8].                  */
	/* Section [0,8] is governed by a ramped tempo point at beat 0 whose */
	/* end tempo is the (constant) 240 point at beat 8.                 */
	/* ---------------------------------------------------------------- */
	{
		TempoMap map;
		map.set_sample_rate (48000);
		map.set_meter (4, 4);
		map.add_tempo (0.0, 120.0, true);   /* ramp begins at beat 0 */
		map.add_tempo (8.0, 240.0, false);  /* reaches 240 at beat 8 */

		const double t_start = map.tempo_at_sample (0);
		const int64_t s_end  = map.beats_to_sample (8.0);
		const double t_end   = map.tempo_at_sample (s_end);
		std::printf ("ramp: tempo@start=%.4f  sample@beat8=%lld  tempo@beat8=%.4f\n",
		             t_start, (long long) s_end, t_end);

		check (std::fabs (t_start - 120.0) < 1e-6, "ramp: tempo at start == 120");
		check (std::fabs (t_end - 240.0) < 1e-3, "ramp: instantaneous tempo at beat 8 == 240");

		/* midpoint (in beats) tempo must be strictly between 120 and 240 */
		const int64_t s_mid = map.beats_to_sample (4.0);
		const double t_mid  = map.tempo_at_sample (s_mid);
		std::printf ("ramp: sample@beat4=%lld tempo@beat4=%.4f (120 < t < 240)\n",
		             (long long) s_mid, t_mid);
		check (t_mid > 120.0 && t_mid < 240.0, "ramp: midpoint tempo between 120 and 240");

		/* sample position must be monotonic increasing across the ramp */
		int64_t prev = -1;
		bool monotonic = true;
		for (double bt = 0.0; bt <= 8.0 + 1e-9; bt += 0.5) {
			const int64_t s = map.beats_to_sample (bt);
			if (s < prev) {
				monotonic = false;
				break;
			}
			prev = s;
		}
		check (monotonic, "ramp: beats_to_sample monotonic non-decreasing");

		/* instantaneous tempo must be monotonic increasing (accelerando) */
		double ptempo = -1.0;
		bool tempo_mono = true;
		for (double bt = 0.0; bt < 8.0; bt += 0.5) {
			const int64_t s = map.beats_to_sample (bt);
			const double tt = map.tempo_at_sample (s);
			if (tt < ptempo - 1e-6) {
				tempo_mono = false;
				break;
			}
			ptempo = tt;
		}
		check (tempo_mono, "ramp: tempo_at_sample monotonic non-decreasing");

		/* a ramp reaches beat 8 sooner than a constant 120 would (176400
		 * samples), and later than a constant 240 (88200 samples). */
		std::printf ("ramp: beats_to_sample(8) = %lld (const120=192000, const240=96000)\n",
		             (long long) s_end);
		check (s_end < 192000 && s_end > 96000, "ramp: beat 8 sample between const-120 and const-240");

		/* sample_to_beats is the inverse of beats_to_sample across the ramp */
		const double back = map.sample_to_beats (s_end);
		check (std::fabs (back - 8.0) < 1e-3, "ramp: sample_to_beats(beats_to_sample(8)) ~= 8");
	}

	/* ---------------------------------------------------------------- */
	/* Transport clock on top of a constant 120 BPM / 4/4 map.          */
	/* ---------------------------------------------------------------- */
	{
		TempoMap map;
		map.set_sample_rate (48000);
		map.set_tempo (120.0);
		map.set_meter (4, 4);

		Transport xport (map);
		xport.set_sample_rate (48000);

		check (!xport.rolling(), "transport: not rolling initially");
		check (xport.sample() == 0, "transport: sample starts at 0");

		/* stopped: process does nothing */
		xport.process (48000);
		check (xport.sample() == 0, "transport: stopped process() does not advance");

		xport.start ();
		check (xport.rolling(), "transport: rolling after start()");
		xport.process (48000); /* 1 second */
		check (xport.sample() == 48000, "transport: advanced 48000 after 1s of frames");
		check (std::fabs (xport.beats() - 2.0) < 1e-9, "transport: 1 second == 2 beats @120");

		xport.process (48000); /* another second => beat 4 => bar 2 */
		check (xport.sample() == 96000, "transport: advanced to 96000");
		check (bbt_eq (xport.bbt(), 2, 1, 0), "transport: bbt == bar 2 beat 1");
		check (std::fabs (xport.tempo() - 120.0) < 1e-9, "transport: tempo == 120");

		xport.stop ();
		check (!xport.rolling(), "transport: stopped after stop()");
		xport.locate (0);
		xport.process (0); /* locate is now a pending seek; applied at block start */
		check (xport.sample() == 0, "transport: locate(0) resets playhead");
	}

	/* ---------------------------------------------------------------- */
	/* Integer tick API: round-trip exactness at 500 BPM / 48 kHz.      */
	/* One 192-PPQN tick = 48000*60/(500*192) = 30 samples exactly, so  */
	/* every reachable tick must round-trip EXACTLY.                    */
	/* ---------------------------------------------------------------- */
	{
		TempoMap map;
		map.set_sample_rate (48000);
		map.set_tempo (500.0);
		map.set_meter (4, 4);

		check (map.tick_to_sample (0) == 0, "tickapi: tick 0 == sample 0");
		/* one quarter = seq_ppqn ticks = 60/500 s = 5760 samples.  Expressed in
		   seq_ppqn, not a literal, so raising the sequencer PPQN does not turn
		   this into a test of the wrong note value. */
		check (map.tick_to_sample (seq_ppqn) == 5760, "tickapi: 1 beat @500 == sample 5760");
		check (map.sample_to_tick (5760) == seq_ppqn, "tickapi: sample 5760 == 1 beat");

		int bad = 0;
		long long first_bad = -1;
		for (int64_t t = 0; t <= 10000; ++t) {
			const int64_t s    = map.tick_to_sample (t);
			const int64_t back = map.sample_to_tick (s);
			if (back != t) {
				++bad;
				if (first_bad < 0) {
					first_bad = (long long) t;
				}
			}
		}
		std::printf ("tickapi: round-trip mismatches = %d / 10001 (first at tick %lld)\n", bad, first_bad);
		check (bad == 0, "tickapi: sample_to_tick(tick_to_sample(t)) == t for ALL ticks 0..10000 @500bpm/48k");
	}

	/* ---------------------------------------------------------------- */
	/* 32nd-note grid @ 500 BPM / 48 kHz: a 32nd = 24 ticks (192/8) and */
	/* exactly 48000*60/(500*8) = 720 samples. The inter-onset sequence */
	/* must be jitter-free and accumulate zero drift over 10k notes.    */
	/* ---------------------------------------------------------------- */
	{
		TempoMap map;
		map.set_sample_rate (48000);
		map.set_tempo (500.0);
		map.set_meter (4, 4);

		int64_t prev  = map.tick_to_sample (0);
		int64_t min_d = INT64_MAX;
		int64_t max_d = INT64_MIN;
		int bad_ioi = 0;
		/* a 32nd note is an eighth of a beat, whatever the PPQN */
		const int64_t t32 = seq_ppqn / 8;
		for (int n = 1; n <= 10000; ++n) {
			const int64_t s = map.tick_to_sample ((int64_t) n * t32);
			const int64_t d = s - prev;
			if (d < 720 || d > 721) {
				++bad_ioi;
			}
			if (d < min_d) { min_d = d; }
			if (d > max_d) { max_d = d; }
			prev = s;
		}
		std::printf ("32nd grid: IOI min=%lld max=%lld bad=%d over 10000 notes\n",
		             (long long) min_d, (long long) max_d, bad_ioi);
		check (bad_ioi == 0, "32nd grid: every IOI is 720 or 721 samples over 10k notes");
		check (max_d - min_d <= 1, "32nd grid: jitter never exceeds 1 sample");
		check (min_d == 720 && max_d == 720, "32nd grid: EXACTLY 720 samples per 32nd at 48k (acceptance)");
		check (map.tick_to_sample (t32 * 10000) == 7200000LL, "32nd grid: zero drift over 10k notes (== sample 7200000)");
	}

	/* ---------------------------------------------------------------- */
	/* copy_with_tempo_at: 120 bpm map, live edit to 500 at beat 100.   */
	/* History before beat 100 identical; after, 500 bpm; original      */
	/* untouched. beat 100 @120 = 50 s = 2400000 samples;               */
	/* one beat @500 = 5760 samples.                                    */
	/* ---------------------------------------------------------------- */
	{
		TempoMap map;
		map.set_sample_rate (48000);
		map.set_tempo (120.0);
		map.set_meter (4, 4);

		TempoMap* live = map.copy_with_tempo_at (100.0, 500.0);

		bool same_before = true;
		for (int64_t t = 0; t <= (int64_t) seq_ppqn * 100; t += 7) {  /* up to beat 100 */
			if (live->tick_to_sample (t) != map.tick_to_sample (t)) {
				same_before = false;
				break;
			}
		}
		check (same_before, "copy: every tick position before/at beat 100 identical to original");
		check (live->beats_to_sample (100.0) == 2400000, "copy: beat 100 boundary == sample 2400000 (unchanged)");
		check (live->beats_to_sample (101.0) == 2400000 + 5760, "copy: beat 101 == +5760 samples (500 bpm)");
		check (live->tick_to_sample ((int64_t) seq_ppqn * 110) == 2400000 + 10 * 5760, "copy: beat 110 follows 500 bpm exactly");
		check (std::fabs (live->tempo_at_sample (2400000 - 100) - 120.0) < 1e-9, "copy: tempo just before beat 100 == 120");
		check (std::fabs (live->tempo_at_sample (2400000 + 100) - 500.0) < 1e-9, "copy: tempo after beat 100 == 500");
		check (std::fabs (map.tempo_at_sample (7200000) - 120.0) < 1e-9, "copy: original map untouched (still 120)");
		delete live;
	}

	/* copy_with_tempo_at preserves MULTI-POINT history before the edit. */
	{
		TempoMap map;
		map.set_sample_rate (48000);
		map.set_meter (4, 4);
		map.add_tempo (0.0, 120.0, false);
		map.add_tempo (50.0, 240.0, false);   /* beat 50 = 25 s = 1200000 */

		TempoMap* live = map.copy_with_tempo_at (100.0, 500.0);

		check (live->beats_to_sample (25.0) == map.beats_to_sample (25.0), "copy multi: 120-section (beat 25) preserved");
		check (live->beats_to_sample (75.0) == map.beats_to_sample (75.0), "copy multi: 240-section (beat 75) preserved");
		/* beat 100 = 1200000 + 50 * 12000 = 1800000; beat 101 = +5760 @500 */
		check (live->beats_to_sample (101.0) == 1800000 + 5760, "copy multi: beat 101 == +5760 samples (500 bpm)");
		delete live;
	}

	/* ---------------------------------------------------------------- */
	/* Pending seek: applied at process() START, consumed exactly once. */
	/* ---------------------------------------------------------------- */
	{
		TempoMap map;
		map.set_sample_rate (48000);
		map.set_tempo (120.0);
		map.set_meter (4, 4);

		Transport xport (map);
		xport.set_sample_rate (48000);

		/* seek while stopped: deferred until process(), then no advance */
		xport.request_seek (48000);
		check (xport.sample() == 0, "seek: pending seek NOT applied before process()");
		xport.process (512);
		check (xport.sample() == 48000, "seek: applied at process() start while stopped (no advance)");

		/* seek while rolling: applied at block START, then block advances */
		xport.start ();
		xport.request_seek (96000);
		xport.process (512);
		check (xport.sample() == 96000 + 512, "seek: applied at block start, then block advances");

		/* consumed exactly once */
		xport.process (512);
		check (xport.sample() == 96000 + 1024, "seek: consumed once (next block just advances)");

		/* negative target clamps to 0; applied even for a zero-frame block */
		xport.request_seek (-5);
		xport.process (0);
		check (xport.sample() == 0, "seek: negative target clamps to 0 (applied on process(0))");

		/* locate() is unified through request_seek */
		xport.locate (777);
		check (xport.sample() == 0, "seek: locate() deferred until block start");
		xport.process (0);
		check (xport.sample() == 777, "seek: locate() applied at next process()");
		xport.stop ();
	}

	std::printf ("\n%s (%d failure%s)\n",
	             g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
	             g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
