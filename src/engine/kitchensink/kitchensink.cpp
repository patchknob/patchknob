/*
 * kitchensink.cpp - implementation.
 *
 * The timing algorithms below are ported from Ardour's GPL `temporal` library
 * (libs/temporal/tempo.cc, bbt_time.cc, and the associated headers),
 * Copyright (C) 2017-2021 Paul Davis. See kitchensink.h for the full note.
 *
 * GPL v2+; see kitchensink.h.
 */

#include "kitchensink.h"

#include <algorithm>
#include <cstdlib>

namespace kitchensink {

/* ------------------------------------------------------------------------- */
/* constants + low-level helpers (ported from temporal/superclock.* and
 * pbd/integer_division.h)                                                    */
/* ------------------------------------------------------------------------- */

/* 282240000 = 2^10 * 3^2 * 5^4 * 7^2  (Temporal::_superclock_ticks_per_second) */
static const superclock_t SCTS = 282240000;

/* muldiv_round / muldiv_floor: compute v * (n/d) in 128-bit precision so that
 * the intermediate product never overflows. Ported from PBD::muldiv_*.
 * GCC/Clang __int128 is used (available on the mingw64 toolchain).
 */
static inline int64_t muldiv_round (int64_t v, int64_t n, int64_t d)
{
	__int128 vn = (__int128) v * (__int128) n;
	/* PBD_IDIV_ROUNDING: same sign as vn, magnitude d/2 */
	const __int128 asr = (((vn ^ (__int128) d) < 0) ? (__int128) -1 : (__int128) 0);
	const __int128 rounding = ((__int128) (d / 2)) - (asr & (__int128) d);
	return (int64_t) ((vn + rounding) / (__int128) d);
}

static inline superclock_t superclock_to_samples (superclock_t s, int64_t sr)
{
	/* SYMMETRIC with samples_to_superclock below: muldiv_round in BOTH
	 * directions (this used to floor, biasing every position up to one
	 * sample early and breaking sample<->superclock round trips). */
	return muldiv_round (s, sr, SCTS);
}

static inline superclock_t samples_to_superclock (int64_t samples, int64_t sr)
{
	return muldiv_round (samples, SCTS, sr);
}

/* superclocks per note type, from note-types-per-minute.
 * (Temporal::Tempo::double_npm_to_scpn)
 */
static inline superclock_t npm_to_scpn (double npm)
{
	return (superclock_t) std::llround ((60.0 / npm) * (double) SCTS);
}

namespace detail {

/* ------------------------------------------------------------------------- */
/* TempoPoint - tempo domain conversions (ported from Temporal::TempoPoint)   */
/* ------------------------------------------------------------------------- */

/* beats -> superclocks, integrating a constant or ramped tempo.
 * Ported from TempoPoint::superclock_at().
 */
superclock_t
TempoPoint::superclock_at (const Beats& qn) const
{
	if (qn.to_ticks() == beats_ticks) {
		return sclock;
	}

	const Beats delta = qn - beats();

	if (omega == 0.0) {
		/* not ramped: linear. r = sc + spqn*whole_beats + spqn*ticks/PPQN */
		return sclock
			+ (scpqn * delta.get_beats())
			+ muldiv_round (scpqn, delta.get_ticks(), (int64_t) Beats::PPQN);
	}

	/* ramped: closed form using the log1p of the ramp. See Ardour doc/tempo. */
	const double log_expr = (double) scpqn * omega * delta.to_double();
	return sclock + (superclock_t) std::llrint (std::log1p (log_expr) / omega);
}

/* superclocks -> beats. Ported from TempoPoint::quarters_at_superclock().
 * The constant path here uses exact integer math (the inverse of the linear
 * superclock_at above); Ardour's constant path uses a more elaborate
 * "super note type per second" integer expansion, which we simplify to the
 * exact muldiv inverse. The ramped path is the same exp() closed form.
 */
Beats
TempoPoint::quarters_at_superclock (superclock_t sc) const
{
	if (omega == 0.0) {
		const int64_t dticks = muldiv_round (sc - sclock, (int64_t) Beats::PPQN, scpqn);
		return Beats::ticks (beats_ticks + dticks);
	}

	const double b = (std::exp (omega * (double) (sc - sclock)) - 1.0) / ((double) scpqn * omega);
	return beats() + Beats::from_double (b);
}

/* instantaneous tempo (note types per minute) at a superclock position.
 * For a ramp, bpm(sc) = bpm_start * exp(omega * (sc - sclock)); this is the
 * algebraic equivalent of Ardour's
 *   note_types_per_minute_at = scts*60 / (scpn * exp(-omega*dt)).
 */
double
TempoPoint::bpm_at (superclock_t sc) const
{
	if (omega == 0.0) {
		return tempo.bpm;
	}
	return tempo.bpm * std::exp (omega * (double) (sc - sclock));
}

/* ------------------------------------------------------------------------- */
/* MeterPoint - meter <-> BBT math (ported from Temporal::Meter/MeterPoint)   */
/* ------------------------------------------------------------------------- */

int32_t
MeterPoint::ticks_per_grid () const
{
	/* ticks (of PPQN) that make up one division of this meter */
	return (4 * Beats::PPQN) / meter.note_value;
}

/* BBT_Offset -> Beats. Ported from Meter::to_quarters(). */
Beats
MeterPoint::to_quarters (int64_t bars, int64_t beats, int64_t oticks) const
{
	int64_t ticks = ((int64_t) Beats::PPQN * bars * meter.divisions_per_bar * 4) / meter.note_value;
	ticks += ((int64_t) Beats::PPQN * beats * 4) / meter.note_value;

	const int64_t tpg = ticks_per_grid ();

	if (oticks > tpg) {
		ticks += (int64_t) Beats::PPQN * oticks / tpg;
		ticks += oticks % tpg;
	} else {
		ticks += oticks;
	}

	return Beats (ticks / Beats::PPQN, ticks % Beats::PPQN);
}

/* BBT + BBT_Offset -> BBT (normalized to this meter). Ported from
 * Meter::bbt_add(). Internal accumulation uses int64 to tolerate large tick
 * offsets; the stored BBT fields are int32.
 */
BBT
MeterPoint::bbt_add (const BBT& bbt, int64_t abars, int64_t abeats, int64_t aticks) const
{
	const int64_t dpb = meter.divisions_per_bar;

	int64_t bars  = bbt.bars;
	int64_t beats = bbt.beats;
	int64_t ticks = bbt.ticks;

	if ((bars ^ abars) < 0) {
		if (std::llabs (abars) >= std::llabs (bars)) {
			if (bars < 0) { bars++; } else { bars--; }
		}
	}
	if ((beats ^ abeats) < 0) {
		if (std::llabs (abeats) >= std::llabs (beats)) {
			if (beats < 0) { beats++; } else { beats--; }
		}
	}

	int64_t rbars  = bars  + abars;
	int64_t rbeats = beats + abeats;
	int64_t rticks = ticks + aticks;

	const int64_t tpg = ticks_per_grid ();

	if (rticks >= tpg) {
		const int64_t tpB = tpg * dpb; /* ticks per bar */
		if (rticks >= tpB) {
			rbars  += rticks / tpB;
			rticks %= tpB;
		}
		if (rticks >= tpg) {
			rbeats += rticks / tpg;
			rticks %= tpg;
		}
	}

	if (rbeats > dpb) {
		rbeats -= 1;
		rbars  += rbeats / dpb;
		rbeats %= dpb;
		rbeats += 1;
	}

	if (rbars == 0) {
		rbars = 1;
	}

	BBT out;
	out.bars  = (int32_t) rbars;
	out.beats = (int32_t) rbeats;
	out.ticks = (int32_t) rticks;
	return out;
}

/* signed BBT distance between two BBT times under this meter. Ported from
 * Meter::bbt_delta(). Assumes later >= earlier.
 */
void
MeterPoint::bbt_delta (const BBT& later, const BBT& earlier,
                       int64_t& dbars, int64_t& dbeats, int64_t& dticks) const
{
	const int64_t dpb = meter.divisions_per_bar;

	if (later.bars == earlier.bars && later.beats == earlier.beats && later.ticks == earlier.ticks) {
		dbars = dbeats = dticks = 0;
		return;
	}

	BBT a = earlier;
	BBT b = later;

	if (a.ticks > b.ticks) {
		dticks = b.ticks + (ticks_per_grid() - a.ticks);
		if (a.beats == dpb) {
			a.beats = 1;
			a.bars++;
		} else {
			a.beats++;
		}
	} else {
		dticks = b.ticks - a.ticks;
	}

	if (a.beats > b.beats) {
		dbeats = b.beats + (dpb - a.beats);
		a.bars++;
	} else {
		dbeats = b.beats - a.beats;
	}

	dbars = b.bars - a.bars;
}

/* BBT -> beats. Ported from MeterPoint::quarters_at(). */
Beats
MeterPoint::quarters_at (const BBT& bbt) const
{
	int64_t db, dbe, dt;
	bbt_delta (bbt, this->bbt, db, dbe, dt);
	return beats() + to_quarters (db, dbe, dt);
}

/* beats -> BBT. Ported from MeterPoint::bbt_at(). */
BBT
MeterPoint::bbt_at (const Beats& qn) const
{
	const int64_t dt = (qn - beats()).to_ticks();
	return bbt_add (this->bbt, 0, 0, dt);
}

} /* namespace detail */

using detail::Beats;
using detail::TempoPoint;
using detail::MeterPoint;

/* ------------------------------------------------------------------------- */
/* TempoMap                                                                   */
/* ------------------------------------------------------------------------- */

TempoMap::TempoMap ()
	: _sr (48000.0)
{
	clear ();
}

void
TempoMap::clear ()
{
	_tempos.clear ();
	_meters.clear ();

	TempoPoint tp;               /* default 120 bpm, quarter note, constant */
	tp.tempo = Tempo (120.0, 4);
	tp.ramp = false;
	tp.beats_ticks = 0;
	_tempos.push_back (tp);

	MeterPoint mp;               /* default 4/4 at bar 1 */
	mp.meter = Meter (4, 4);
	mp.bar_pos = 1;
	_meters.push_back (mp);

	recompute ();
}

void
TempoMap::set_sample_rate (double sr)
{
	/* superclock positions are sample-rate independent, so no cache rebuild
	 * is required; only sample<->superclock conversions depend on sr.
	 */
	_sr = sr;
}

void
TempoMap::set_tempo (double bpm)
{
	_tempos.clear ();
	TempoPoint tp;
	tp.tempo = Tempo (bpm, 4);
	tp.ramp = false;
	tp.beats_ticks = 0;
	_tempos.push_back (tp);
	recompute ();
}

void
TempoMap::set_meter (int div_per_bar, int note_value)
{
	_meters.clear ();
	MeterPoint mp;
	mp.meter = Meter (div_per_bar, note_value);
	mp.bar_pos = 1;
	_meters.push_back (mp);
	recompute ();
}

void
TempoMap::add_tempo (double at_beat, double bpm, bool ramp)
{
	const int64_t bt = (int64_t) std::llrint (at_beat * detail::Beats::PPQN);

	for (auto& t : _tempos) {
		if (t.beats_ticks == bt) {
			/* overwrite an existing tempo at this position */
			t.tempo = Tempo (bpm, 4);
			t.ramp = ramp;
			recompute ();
			return;
		}
	}

	TempoPoint tp;
	tp.tempo = Tempo (bpm, 4);
	tp.ramp = ramp;
	tp.beats_ticks = bt;
	_tempos.push_back (tp);
	recompute ();
}

void
TempoMap::add_meter (int at_bar, int div_per_bar, int note_value)
{
	for (auto& m : _meters) {
		if (m.bar_pos == at_bar) {
			m.meter = Meter (div_per_bar, note_value);
			recompute ();
			return;
		}
	}

	MeterPoint mp;
	mp.meter = Meter (div_per_bar, note_value);
	mp.bar_pos = at_bar;
	_meters.push_back (mp);
	recompute ();
}

/* Rebuild all cached positions (superclock, beats, BBT) and ramp coefficients.
 * This mirrors, in a single pass, what Ardour's reset_starting_at() does
 * incrementally.
 */
void
TempoMap::recompute ()
{
	if (_tempos.empty()) {
		TempoPoint tp; tp.tempo = Tempo (120.0, 4); _tempos.push_back (tp);
	}
	if (_meters.empty()) {
		MeterPoint mp; mp.meter = Meter (4, 4); _meters.push_back (mp);
	}

	std::sort (_tempos.begin(), _tempos.end(),
	           [](const TempoPoint& a, const TempoPoint& b) { return a.beats_ticks < b.beats_ticks; });
	std::sort (_meters.begin(), _meters.end(),
	           [](const MeterPoint& a, const MeterPoint& b) { return a.bar_pos < b.bar_pos; });

	/* Pass A: meter beat positions + BBT. Meters partition the beat timeline
	 * into bars independently of tempo. */
	_meters[0].beats_ticks = 0;
	_meters[0].bar_pos = 1;
	_meters[0].bbt = BBT {1, 1, 0};
	for (size_t i = 1; i < _meters.size(); ++i) {
		MeterPoint& prev = _meters[i-1];
		MeterPoint& cur  = _meters[i];
		const int64_t nbars = (int64_t) cur.bar_pos - prev.bar_pos;
		const int64_t ticks_per_bar = (int64_t) prev.meter.divisions_per_bar * prev.ticks_per_grid();
		cur.beats_ticks = prev.beats_ticks + nbars * ticks_per_bar;
		cur.bbt = BBT { cur.bar_pos, 1, 0 };
	}

	/* Pass B: per-tempo superclocks-per-quarter-note + ramp omega. */
	for (auto& t : _tempos) {
		const superclock_t scpnt = npm_to_scpn (t.tempo.bpm);
		t.scpqn = (scpnt * t.tempo.note_type) / 4; /* superclocks_per_note_type(4) */
	}
	for (size_t i = 0; i < _tempos.size(); ++i) {
		TempoPoint& t = _tempos[i];
		if (t.ramp && (i + 1) < _tempos.size()) {
			const superclock_t scpqn     = t.scpqn;
			const superclock_t end_scpqn = _tempos[i+1].scpqn;
			const double qd = (double) (_tempos[i+1].beats_ticks - t.beats_ticks) / (double) detail::Beats::PPQN;
			if (scpqn == end_scpqn || qd <= 0.0) {
				t.omega = 0.0;
			} else {
				/* Ported from TempoPoint::compute_omega_from_quarter_duration */
				t.omega = ((1.0 / end_scpqn) - (1.0 / scpqn)) / qd;
			}
			t.tempo.end_bpm = _tempos[i+1].tempo.bpm;
		} else {
			t.omega = 0.0;
			t.tempo.end_bpm = t.tempo.bpm;
		}
	}

	/* Pass C: walk tempo + meter points in beat order, computing each point's
	 * superclock (from the tempo governing the preceding interval) and BBT
	 * (from the meter in effect). */
	_tempos[0].sclock = 0;
	_tempos[0].beats_ticks = 0;
	_tempos[0].bbt = BBT {1, 1, 0};
	_meters[0].sclock = 0;

	const TempoPoint* active_tempo = &_tempos[0];
	const MeterPoint* active_meter = &_meters[0];

	size_t i = 1; /* next tempo  */
	size_t j = 1; /* next meter  */

	while (i < _tempos.size() || j < _meters.size()) {
		bool take_meter;
		if (i >= _tempos.size()) {
			take_meter = true;
		} else if (j >= _meters.size()) {
			take_meter = false;
		} else {
			/* on a tie, place the meter first so a coincident tempo sees it */
			take_meter = (_meters[j].beats_ticks <= _tempos[i].beats_ticks);
		}

		if (take_meter) {
			MeterPoint& mp = _meters[j];
			mp.sclock = active_tempo->superclock_at (Beats::ticks (mp.beats_ticks));
			/* mp.bbt already {bar,1,0} from Pass A */
			active_meter = &mp;
			++j;
		} else {
			TempoPoint& tp = _tempos[i];
			tp.sclock = active_tempo->superclock_at (Beats::ticks (tp.beats_ticks));
			tp.bbt    = active_meter->bbt_at (Beats::ticks (tp.beats_ticks));
			active_tempo = &tp;
			++i;
		}
	}
}

const TempoPoint&
TempoMap::tempo_at_sclock (superclock_t sc) const
{
	const TempoPoint* ret = &_tempos.front();
	for (const auto& t : _tempos) {
		if (t.sclock <= sc) {
			ret = &t;
		} else {
			break;
		}
	}
	return *ret;
}

const TempoPoint&
TempoMap::tempo_at_beats (const Beats& b) const
{
	const TempoPoint* ret = &_tempos.front();
	for (const auto& t : _tempos) {
		if (t.beats_ticks <= b.to_ticks()) {
			ret = &t;
		} else {
			break;
		}
	}
	return *ret;
}

const MeterPoint&
TempoMap::meter_at_beats (const Beats& b) const
{
	const MeterPoint* ret = &_meters.front();
	for (const auto& m : _meters) {
		if (m.beats_ticks <= b.to_ticks()) {
			ret = &m;
		} else {
			break;
		}
	}
	return *ret;
}

const MeterPoint&
TempoMap::meter_at_bbt (const BBT& bbt) const
{
	const MeterPoint* ret = &_meters.front();
	for (const auto& m : _meters) {
		if (m.bar_pos <= bbt.bars) {
			ret = &m;
		} else {
			break;
		}
	}
	return *ret;
}

double
TempoMap::sample_to_beats (int64_t sample) const
{
	const superclock_t sc = samples_to_superclock (sample, (int64_t) _sr);
	return tempo_at_sclock (sc).quarters_at_superclock (sc).to_double();
}

int64_t
TempoMap::beats_to_sample (double beats) const
{
	const Beats b = Beats::from_double (beats);
	const superclock_t sc = tempo_at_beats (b).superclock_at (b);
	return superclock_to_samples (sc, (int64_t) _sr);
}

BBT
TempoMap::beats_to_bbt (double beats) const
{
	const Beats b = Beats::from_double (beats);
	return meter_at_beats (b).bbt_at (b);
}

double
TempoMap::bbt_to_beats (const BBT& bbt) const
{
	return meter_at_bbt (bbt).quarters_at (bbt).to_double();
}

BBT
TempoMap::sample_to_bbt (int64_t sample) const
{
	const superclock_t sc = samples_to_superclock (sample, (int64_t) _sr);
	const Beats b = tempo_at_sclock (sc).quarters_at_superclock (sc);
	return meter_at_beats (b).bbt_at (b);
}

double
TempoMap::tempo_at_sample (int64_t sample) const
{
	const superclock_t sc = samples_to_superclock (sample, (int64_t) _sr);
	return tempo_at_sclock (sc).bpm_at (sc);
}

/* Integer sequencer-tick conversions. PatchKnob runs at 192 PPQN; the internal
 * Beats domain is 1920 PPQN, so 192-ticks map EXACTLY (x10) onto Beats ticks
 * with no double quantization (Beats::from_double never enters this path).
 * Both directions round symmetrically (muldiv_round), so
 * sample_to_tick (tick_to_sample (t)) == t for every reachable tick: the
 * half-sample reconstruction error is far below half a 192-tick at any
 * musical tempo (one 192-tick = 30 samples at 500 BPM / 48 kHz).
 * RT-safe: no locks, no allocation (linear scan of the tiny point vector).
 */
int64_t
TempoMap::tick_to_sample (int64_t tick_192) const
{
	const Beats b = Beats::ticks (tick_192 * 10);
	const superclock_t sc = tempo_at_beats (b).superclock_at (b);
	return superclock_to_samples (sc, (int64_t) _sr);
}

int64_t
TempoMap::sample_to_tick (int64_t sample) const
{
	const superclock_t sc = samples_to_superclock (sample, (int64_t) _sr);
	const int64_t t1920 = tempo_at_sclock (sc).quarters_at_superclock (sc).to_ticks ();
	/* nearest 192-PPQN tick, same symmetric rounding */
	return muldiv_round (t1920, 1, 10);
}

/* RCU re-anchor for LIVE tempo edits: heap copy keeping tempo history strictly
 * before `at_beat`, holding `bpm` from `at_beat` on. The message thread builds
 * this copy, publishes the pointer atomically, and retires the old map after a
 * block-generation grace period; positions before `at_beat` are unchanged, so
 * the playhead's musical position does not teleport at the edit.
 */
TempoMap*
TempoMap::copy_with_tempo_at (double at_beat, double bpm) const
{
	TempoMap* map = new TempoMap (*this);

	const int64_t bt = (int64_t) std::llrint (at_beat * (double) detail::Beats::PPQN);

	/* truncate every tempo point at or after at_beat */
	map->_tempos.erase (std::remove_if (map->_tempos.begin(), map->_tempos.end(),
	                                    [bt](const TempoPoint& t) { return t.beats_ticks >= bt; }),
	                    map->_tempos.end());

	/* hold bpm from at_beat on (add_tempo sorts + recomputes) */
	map->add_tempo (at_beat, bpm, false);

	return map;
}

/* ------------------------------------------------------------------------- */
/* Transport                                                                  */
/* ------------------------------------------------------------------------- */

Transport::Transport (const TempoMap& map)
	: _map (&map)
	, _sr (map.sample_rate())
	, _sample (0)
	, _rolling (false)
{
}

void
Transport::start ()
{
	_rolling.store (true, std::memory_order_relaxed);
}

void
Transport::stop ()
{
	_rolling.store (false, std::memory_order_relaxed);
}

void
Transport::locate (int64_t sample)
{
	/* unified with request_seek: process() applies the seek at the START of
	 * the next block, so a locate can never move the playhead mid-block
	 * underneath the renderer (a direct store here raced process()). */
	request_seek (sample);
}

void
Transport::process (int nframes)
{
	/* FIRST: apply any pending seek, before anything reads the position for
	 * this block. exchange() consumes it exactly once. */
	const int64_t seek = _pendingSeek.exchange (-1, std::memory_order_acq_rel);
	if (seek >= 0) {
		_sample.store (seek, std::memory_order_relaxed);
	}
	if (nframes <= 0) {
		return;
	}
	if (_rolling.load (std::memory_order_relaxed)) {
		_sample.fetch_add ((int64_t) nframes, std::memory_order_relaxed);
	}
}

double
Transport::beats () const
{
	return _map->sample_to_beats (sample());
}

BBT
Transport::bbt () const
{
	return _map->sample_to_bbt (sample());
}

double
Transport::tempo () const
{
	return _map->tempo_at_sample (sample());
}

} /* namespace kitchensink */
