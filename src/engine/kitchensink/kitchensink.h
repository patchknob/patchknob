/*
 * kitchensink - a self-contained musical tempo map + transport clock library.
 *
 * The timing math in this library (tempo/meter/BBT/superclock conversions and
 * the exponential tempo-ramp integration) is a PORT of the algorithms in
 * Ardour's GPL `temporal` library:
 *
 *     ardour/libs/temporal/{tempo.cc, tempo.h, bbt_time.{h,cc},
 *                           beats.h, superclock.h, types.h}
 *     Copyright (C) 2017-2021 Paul Davis
 *
 * All of the PBD::Stateful / XML / event-signal / boost / i18n machinery
 * from the original has been dropped; only the pure integer/float timing
 * formulas were reused. Credit and copyright for those formulas belongs to
 * Ardour's authors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#pragma once

#include <cstdint>
#include <vector>
#include <cmath>
#include <atomic>

namespace kitchensink {

/* A very high resolution clock whose frequency has as factors all common
 * sample rates and note-type divisors. This is Ardour's superclock, and its
 * tick rate (282240000 = 2^10 * 3^2 * 5^4 * 7^2) is copied verbatim so the
 * integer math produces identical results.
 */
typedef int64_t superclock_t;

/* Smallest division of a beat. Many integer factors, so 1/Nth beat divisions
 * land on integer tick counts. (Temporal::ticks_per_beat)
 *
 * 3840 = 2^8 * 3 * 5, raised from Ardour's 1920 for two reasons:
 *   * it keeps the sequencer<->Beats conversion EXACT.  c_ppqn is 768 and
 *     3840 / 768 == 5, an integer ratio, so tick_to_sample/sample_to_tick stay
 *     lossless integer muldiv (see TempoMap::tick_to_sample).  1920 could not
 *     do this: 1920 / 768 == 2.5.
 *   * the 2^8 factor is what makes LPB 256 representable at all.  1920 is
 *     2^7 * 3 * 5, so no sequencer PPQN derived from it divides evenly by 256.
 */
static const int32_t ticks_per_beat = 3840;

/* The SEQUENCER's tick resolution (what the app calls c_ppqn, and what every
 * stored event timestamp is measured in).  It lives here, next to the Beats
 * domain it has to stay commensurate with, so the two constants cannot drift
 * apart in separate headers -- the engine must not include the app's globals.h,
 * so globals.h asserts against THIS value instead.
 *
 * 768 = 2^8 * 3.  The 2^8 is what allows tracker LPB values up to 256 to divide
 * it exactly (768/256 == 3 ticks per row); the 3 keeps triplet grids (LPB 3, 6,
 * 12, 24, 48, 96, 384) exact as well.
 */
static const int32_t seq_ppqn = 768;

/* Bar / Beat / Tick time. bars and beats are 1-based; the neutral value is
 * 1|1|0. ticks run 0..(ticks_per_beat*4/note_value - 1) within a beat.
 */
struct BBT {
	int32_t bars;
	int32_t beats;
	int32_t ticks;
};

/** Tempo: the speed at which musical time progresses.
 *
 * `bpm` is the number of `note_type` notes per minute at the START of the
 * section; `end_bpm` is the value at the END. When they are equal the tempo is
 * CONSTANT; when they differ it is RAMPED (an exponential ramp, see
 * TempoMap's omega coefficient). note_type defaults to 4 (a quarter note),
 * exactly as Ardour models it.
 */
struct Tempo {
	double bpm;      /* note types per minute, start */
	double end_bpm;  /* note types per minute, end   */
	int    note_type;

	enum Type { Constant, Ramped };

	Tempo (double npm = 120.0, int nt = 4)
		: bpm (npm), end_bpm (npm), note_type (nt) {}
	Tempo (double npm, double enpm, int nt = 4)
		: bpm (npm), end_bpm (enpm), note_type (nt) {}

	bool ramped () const { return bpm != end_bpm; }
	Type type ()   const { return bpm == end_bpm ? Constant : Ramped; }
};

/** Meter (time signature): how many subdivisions per bar, and which note type
 *  is one subdivision. e.g. 4/4 -> divisions_per_bar=4, note_value=4.
 */
struct Meter {
	int divisions_per_bar;
	int note_value;

	Meter (int dpb = 4, int nv = 4)
		: divisions_per_bar (dpb), note_value (nv) {}
};

namespace detail {

/** Musical time in beats, stored as an integer count of ticks (PPQN=1920 per
 *  quarter note). Ported from Temporal::Beats (integer tick representation is
 *  what keeps the BBT math exact).
 */
class Beats {
	int64_t _ticks;
  public:
	static const int32_t PPQN = ticks_per_beat;

	Beats () : _ticks (0) {}
	Beats (int64_t b, int64_t t) : _ticks (b * PPQN + t) {}

	static Beats ticks (int64_t t) { Beats b; b._ticks = t; return b; }
	static Beats from_double (double beats) {
		double whole;
		const double frac = std::modf (beats, &whole);
		return Beats ((int64_t) whole, (int64_t) std::llrint (frac * PPQN));
	}

	int64_t to_ticks ()  const { return _ticks; }
	int64_t get_beats () const { return _ticks / PPQN; }
	int32_t get_ticks () const { return (int32_t) (_ticks % PPQN); }
	double  to_double () const { return (double) get_beats() + (get_ticks() / (double) PPQN); }

	Beats operator+ (const Beats& o) const { return ticks (_ticks + o._ticks); }
	Beats operator- (const Beats& o) const { return ticks (_ticks - o._ticks); }
	bool  operator<  (const Beats& o) const { return _ticks <  o._ticks; }
	bool  operator<= (const Beats& o) const { return _ticks <= o._ticks; }
	bool  operator>  (const Beats& o) const { return _ticks >  o._ticks; }
	bool  operator== (const Beats& o) const { return _ticks == o._ticks; }
};

/* A cached tempo change. Holds the Tempo plus everything the map precomputes
 * for it: start position in superclocks + beats + BBT, the superclocks per
 * quarter note, and the ramp coefficient `omega` (0 when constant).
 */
struct TempoPoint {
	Tempo        tempo;
	bool         ramp;         /* ramp toward the next tempo point */
	int64_t      beats_ticks;  /* start position (quarter-note beats), in ticks */
	superclock_t sclock;       /* start position in superclocks */
	BBT          bbt;          /* start position in BBT */
	superclock_t scpqn;        /* superclocks per quarter note (start) */
	double       omega;        /* ramp coefficient */

	TempoPoint () : ramp (false), beats_ticks (0), sclock (0), bbt {1,1,0}, scpqn (0), omega (0.0) {}

	Beats beats () const { return Beats::ticks (beats_ticks); }

	/* domain conversions valid while this point's tempo is in effect */
	superclock_t superclock_at (const Beats& qn) const;
	Beats        quarters_at_superclock (superclock_t sc) const;
	double       bpm_at (superclock_t sc) const; /* instantaneous note types/minute */
};

/* A cached meter change. */
struct MeterPoint {
	Meter        meter;
	int          bar_pos;      /* 1-based bar at which this meter begins */
	int64_t      beats_ticks;  /* start position (quarter-note beats), in ticks */
	superclock_t sclock;       /* start position in superclocks */
	BBT          bbt;          /* start position in BBT ( {bar_pos,1,0} ) */

	MeterPoint () : bar_pos (1), beats_ticks (0), sclock (0), bbt {1,1,0} {}

	Beats beats () const { return Beats::ticks (beats_ticks); }

	int32_t ticks_per_grid () const;                 /* ticks (of PPQN) per meter division */
	Beats   to_quarters (int64_t bars, int64_t beats, int64_t ticks) const;
	BBT     bbt_add (const BBT& bbt, int64_t bars, int64_t beats, int64_t ticks) const;
	void    bbt_delta (const BBT& later, const BBT& earlier,
	                   int64_t& dbars, int64_t& dbeats, int64_t& dticks) const;
	Beats   quarters_at (const BBT& bbt) const;      /* BBT -> beats */
	BBT     bbt_at (const Beats& qn) const;          /* beats -> BBT */
};

} /* namespace detail */

/** Tempo Map: a timeline of tempo and meter changes keyed by musical position.
 *
 * Modeled on Ardour's TempoMap: a sorted list of tempo/meter points, each
 * caching its start superclock + start beat + (for tempo) the ramp
 * coefficient. Conversions integrate ramps in closed form (never per-sample
 * stepping).
 *
 * Editing methods (set_*, add_*, clear) must NOT be called concurrently with
 * the conversion methods; edit the map when the transport is stopped.
 */
class TempoMap {
  public:
	TempoMap ();

	void set_sample_rate (double sr);
	double sample_rate () const { return _sr; }

	/* simple single-value setters (replace all tempo / all meter) */
	void set_tempo (double bpm);
	void set_meter (int div_per_bar, int note_value);

	/* incremental changes */
	void add_tempo (double at_beat, double bpm, bool ramp);
	void add_meter (int at_bar, int div_per_bar, int note_value);

	void clear ();

	/* sample-accurate conversions honoring ramps + meter changes */
	double  sample_to_beats (int64_t sample) const;
	int64_t beats_to_sample (double beats) const;
	BBT     beats_to_bbt (double beats) const;
	double  bbt_to_beats (const BBT& bbt) const;
	BBT     sample_to_bbt (int64_t sample) const;
	double  tempo_at_sample (int64_t sample) const; /* instantaneous BPM */

	/* integer sequencer-tick conversions (192-PPQN PatchKnob ticks; exact x10 into
	 * the internal 1920-PPQN Beats domain, symmetric rounding both directions
	 * so tick->sample->tick round-trips).  RT-safe: no locks, no allocation. */
	int64_t tick_to_sample (int64_t tick_192) const;
	int64_t sample_to_tick (int64_t sample) const;

	/* Build a heap copy of this map that keeps its history up to `at_beat` and
	 * holds `bpm` from there on: the RCU re-anchor for LIVE tempo edits (the
	 * message thread builds the copy, publishes it atomically, and retires the
	 * old map after a block-generation grace period). */
	TempoMap* copy_with_tempo_at (double at_beat, double bpm) const;

  private:
	double _sr;
	std::vector<detail::TempoPoint> _tempos; /* sorted by beats_ticks */
	std::vector<detail::MeterPoint> _meters; /* sorted by bar_pos     */

	void recompute ();

	const detail::TempoPoint& tempo_at_sclock (superclock_t sc) const;
	const detail::TempoPoint& tempo_at_beats (const detail::Beats& b) const;
	const detail::MeterPoint& meter_at_beats (const detail::Beats& b) const;
	const detail::MeterPoint& meter_at_bbt (const BBT& bbt) const;
};

/** Transport: a play clock on top of a (const) TempoMap.
 *
 * The audio thread calls process() to advance the playhead; the UI thread
 * reads sample()/beats()/bbt()/tempo(). The shared playhead is a plain atomic
 * so those reads are safe. The referenced TempoMap must stay immutable while
 * the transport is rolling (edit it only when stopped).
 */
class Transport {
  public:
	explicit Transport (const TempoMap& map);

	void set_sample_rate (double sr) { _sr = sr; }
	double sample_rate () const { return _sr; }

	void start ();
	void stop ();
	void locate (int64_t sample);

	/* Race-free seek: stashes the target; process() applies it at the START of
	 * the next audio block, before anything reads the position, so a locate can
	 * never move the playhead mid-block under the renderer. */
	void request_seek (int64_t sample) { _pendingSeek.store (sample < 0 ? 0 : sample, std::memory_order_release); }

	/* Seek queued but not yet applied, or -1. */
	int64_t pending_seek () const { return _pendingSeek.load (std::memory_order_acquire); }

	/* Where the playhead is ABOUT to be: the queued seek if one is waiting,
	 * otherwise the live position.  Anything that captures the position to
	 * return to later must use this -- sample() alone reports the pre-seek
	 * value until the audio thread runs a block, so a "locate, then act"
	 * sequence from the UI would otherwise capture the stale position. */
	int64_t effective_sample () const {
		const int64_t seek = pending_seek ();
		return seek >= 0 ? seek : sample ();
	}

	/* Swap the tempo map this transport reads (RCU: caller publishes the new
	 * map and retires the old one after a grace period). */
	void set_map (const TempoMap& map) { _map = &map; }

	bool rolling () const { return _rolling.load (std::memory_order_relaxed); }

	int64_t sample () const { return _sample.load (std::memory_order_relaxed); }
	double  beats () const;
	BBT     bbt () const;
	double  tempo () const;

	/* advance the playhead by nframes samples while rolling (audio thread);
	 * applies any pending seek at block start first.
	 *
	 * NOTE: this is apply_pending_seek() followed by advance(nframes).  It is
	 * only correct when the caller reads the block-start position AFTER calling
	 * it.  A caller that reads sample() at the TOP of a block and calls
	 * process() at the BOTTOM resumes at seekTarget + nframes -- the seek
	 * target's own block is never rendered (bug R2).  Such callers must use the
	 * split pair below instead. */
	void process (int nframes);

	/* --- split form of process(), for callers that read the block-start
	 * position before rendering and advance afterwards (audio thread) --------
	 *
	 *   apply_pending_seek();                  // BEFORE reading sample()
	 *   const int64_t blockStart = sample();   // == the seek target
	 *   ... render [blockStart, blockStart+nframes) ...
	 *   advance (nframes);                     // AFTER rendering
	 *
	 * so the window at the seek target is the very next one rendered. */

	/* Consume any queued seek into the playhead.  Does NOT advance.  Idempotent:
	 * a second call in the same block is a no-op because exchange() takes the
	 * mailbox exactly once.  Returns the sample the playhead was moved to, or
	 * -1 if no seek was pending.  Wait-free (single atomic exchange). */
	int64_t apply_pending_seek ();

	/* Advance the playhead by nframes while rolling.  Does NOT touch the
	 * pending-seek mailbox.  Wait-free. */
	void advance (int nframes);

	/* AUDIO-THREAD ONLY immediate relocate: stores the playhead directly and
	 * deliberately does NOT touch _pendingSeek.  This exists so audio-thread
	 * repositioning (the loop wrap) stops competing with the UI thread for the
	 * single-slot seek mailbox (bug R1): before this, a wrap's request_seek()
	 * could clobber a user locate that had not been consumed yet, or a wrap
	 * could consume the user's locate as if it were its own.
	 *
	 * Because the mailbox is untouched, a user locate that is still pending
	 * survives the wrap and is applied by the next apply_pending_seek() -- the
	 * user's intent correctly wins over the wrap.  Never call this from the UI
	 * thread while the transport is rolling; use request_seek() there. */
	void locate_now (int64_t sample);

  private:
	const TempoMap*       _map;
	double                _sr;
	std::atomic<int64_t>  _sample;
	std::atomic<bool>     _rolling;
	std::atomic<int64_t>  _pendingSeek { -1 };   /* -1 = none */
};

} /* namespace kitchensink */
