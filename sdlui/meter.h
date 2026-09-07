//----------------------------------------------------------------------------
//  sdlui/meter.h -- Ardour's audio level meter, ported to SDL2/SDL3.
//
//  This is a faithful port, not an approximation.  Every curve, every colour
//  stop and both halves of the ballistics come from the Ardour sources
//  vendored next to this tree (../ardour):
//
//    deflection curves   libs/ardour/ardour/logmeter.h
//                        (log_meter, meter_deflect_ppm/_din/_nordic/_vu/_k),
//                        selected per meter type exactly as
//                        gtk2_ardour/meter_patterns.cc::mtr_col_and_fract does.
//    dB conversion       libs/ardour/ardour/dB.h
//                        (dB_to_coefficient, accurate_coefficient_to_dB)
//    falloff ballistics  libs/ardour/meter.cc::PeakMeter::run  (peak power
//                        decays a fixed dB/sec, then max()'d with the new
//                        block peak -> instant attack, linear-dB release)
//                        + libs/ardour/ardour/utils.h falloff rate table.
//    peak hold           libs/widgets/fastmeter.cc::FastMeter::set
//    gradient + stops    gtk2_ardour/level_meter.cc::LevelMeterBase::setup_meters
//                        (the per-type c[10]/stp[4] tables) rendered by
//                        libs/widgets/fastmeter.cc::generate_meter_pattern.
//    bar / hold geometry libs/widgets/fastmeter.cc::vertical_expose,
//                        horizontal_expose.
//
//  The gradient is cached the way FastMeter caches its pixbufs: one small
//  SDL_Texture per (type, length, orientation), so drawing a meter is a single
//  SDL_RenderCopy and drawing sixty of them costs sixty blits.
//
//  Usage per meter, per frame:
//      static ui::meter::State st;
//      ui::meter::update(st, block_peak_linear, seconds_since_last_frame);
//      ui::meter::draw(app.ren, bar_rect, st, ui::meter::Peak, true);
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_SDLUI_METER_H
#define PATCHKNOB_SDLUI_METER_H

#include "gui.h"

namespace ui {
namespace meter {

// Ardour's MeterType, restricted to the scales that have a deflection curve.
//   PPM    = IEC2 (BBC/EBU) PPM        meter_deflect_ppm
//   DIN    = IEC1/DIN                  meter_deflect_din
//   Nordic = IEC1/Nordic               meter_deflect_nordic
//   VU     = VU                        meter_deflect_vu
//   K20/14/12 = K-system               meter_deflect_k(db, krange)
//   Peak   = digital peak, +6 dBFS top log_meter (IEC 60268-18 style scaling)
enum Type { PPM, DIN, Nordic, VU, K20, K14, K12, Peak };

struct State {
    float  peakDb   = -318.f;   // current displayed value in dB
    float  holdDb   = -318.f;   // peak-hold marker in dB
    int    holdFrames = 0;      // hold time remaining, in MILLISECONDS (see note)
    bool   clipped  = false;    // sticky over-0dBFS indicator
};
// NOTE on holdFrames: Ardour counts the hold down in meter refresh ticks
// (FastMeter::hold_cnt, decremented once per FastMeter::set).  That is only a
// time if the refresh rate is fixed, which it is in Ardour (40 ms) and is not
// here, so the counter is kept in milliseconds instead and update() subtracts
// dtSec*1000.  The only meaningful external test is still `> 0` == holding.

// ---- scaling ---------------------------------------------------------------
// dB -> 0..1 deflection along the meter.  Bit-for-bit the corresponding
// meter_deflect_* / log_meter function from logmeter.h.
float deflect(Type t, float db);

// Ardour's meter gradient sampled at `frac` (0 = bottom of the meter, 1 = top):
// green -> bright green -> yellow -> orange -> red, using the "meter color0..9"
// values of Ardour's default dark theme and the Peak/RMS knee positions
// (-18/-9/-3/0 dBFS) from level_meter.cc.
ui::Color color_at(float frac);

// Ardour applies a line-up offset to the level BEFORE deflecting it, per meter
// type (level_meter.cc::update_meters).  This returns that offset for Ardour's
// default configuration, so a caller that wants Ardour's exact calibration can
// use deflect(t, db + default_lineup_db(t)).  draw() does NOT apply it: the
// deflection curves and the printed scales in meter_patterns.cc are both in the
// un-offset space, so leaving it out keeps a meter and its dB scale consistent.
float default_lineup_db(Type t);

// ---- ballistics ------------------------------------------------------------
// peakLin is the block's absolute peak (linear).  dtSec is real elapsed time.
// Instantaneous attack, then a constant dB/sec release -- frame-rate
// independent, unlike a per-frame multiplier.  dtSec is capped at 1 s so a
// stalled or hidden UI resumes rather than snapping the meter to empty.
void update(State& s, float peakLin, double dtSec);
void reset(State& s);

// Release rate in dB/sec, and the peak-hold time in ms.  Defaults are Ardour's
// METER_FALLOFF_MEDIUM (20 dB/s, libs/ardour/ardour/utils.h) and MeterHoldMedium
// (100 refresh ticks x 40 ms = 4000 ms, libs/ardour/ardour/types.h + timers.cc).
extern float falloff_db_per_sec;
extern int   hold_ms;

// ---- drawing ---------------------------------------------------------------
// vertical==true draws bottom-up.  Draws the background, the bar with its
// cached gradient, the peak-hold line and the clip indicator.  No allocation
// and no state on the steady path: one blit for the bar, one for the hold.
// Does NOT draw an outline -- callers frame the box themselves.
void draw(SDL_Renderer* r, const SDL_Rect& box, const State& s, Type t, bool vertical);

// Drop the cached gradient textures.
//
// The shell MUST call this while the renderer is STILL ALIVE and immediately
// before SDL_DestroyRenderer() -- the same reason ui::App::shutdown() releases
// the font atlases there rather than letting ~Font() do it later.  The cache
// holds SDL_Texture* that the renderer owns; once the renderer is destroyed
// they are freed, and this function would then be destroying them a second
// time.  Also call it on a theme change, which invalidates the colours.
//
// Drawing with a renderer other than the one the cache was built against is
// detected and handled internally (the table is forgotten, not destroyed), so a
// missed call leaks nothing and cannot crash -- but a NEW renderer landing on
// the freed address of the old one is indistinguishable from the same renderer,
// and only this call rules that out.
void flush_cache();

// Verifies the ported curves against values computed by hand from Ardour's
// sources.  Returns true when everything matches; prints the mismatches.
bool selftest();

} // namespace meter
} // namespace ui

#endif // PATCHKNOB_SDLUI_METER_H
