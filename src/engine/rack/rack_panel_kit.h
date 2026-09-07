//----------------------------------------------------------------------------
//  rack_panel_kit.h -- the house UI language for rack panels.
//
//  Every control on a PatchKnob module is the SAME vertical unit, so a new
//  module only chooses WHERE to put it, never how it looks:
//
//        (o)     concentric knob -- inner disc = value, outer ring = CV depth
//         |                         (bipolar attenuverter, centred = CV inert)
//         O      the CV jack for that parameter
//         |
//      [CUTOFF]  segment display -- the parameter NAME at rest, its VALUE
//                while the knob or its ring is being turned, then back
//
//  Rules this encodes, all of which were learned the hard way:
//
//    * Reading order is KNOB, JACK, NAME.  A caption under the knob lands
//      BETWEEN the knob and its jack and reads as belonging to neither.
//    * The jack carries no text of its own; the display underneath names the
//      whole unit.
//    * Port captions must set labelPlacement explicitly -- the editor's port
//      path defaults to "input right, output left".
//    * A param can own only ONE panel element (find_element keys on id), so the
//      display gets its own inert `readoutId` and points at the knob it serves
//      through `cvParamId`.
//    * CV rings default CENTRED: a patched jack does nothing until dialled in.
//
//  A module therefore declares, per parameter: the value param, a CV-depth
//  param, an input jack, and one inert readout param.  configControl() wires the
//  first two; addControl() lays out all four.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_RACK_PANEL_KIT_H
#define PATCHKNOB_ENGINE_RACK_PANEL_KIT_H

#include <algorithm>
#include <string>

#include "rack_factory.h"

namespace rackx {
namespace kit {

//! House metrics.  Change here and every module follows.
constexpr float kKnobR      = 17.f;   //!< concentric knob radius
constexpr float kJackR      =  7.f;   //!< CV jack radius
constexpr float kGap        = 10.f;   //!< clear panel between knob and jack
constexpr float kReadoutW   = 72.f;   //!< ~8 mono characters
constexpr float kReadoutH   = 22.f;
//! Knob centre -> jack centre.  Both are drawn at radius+2, so this is
//! (kKnobR + 2) + kGap + (kJackR + 2).
constexpr float kJackDrop   = kKnobR + 2.f + kGap + kJackR + 2.f;
//! Jack centre -> readout centre.
constexpr float kReadoutDrop = kJackDrop + kJackR + 2.f + kGap + kReadoutH * 0.5f;
//! Total height of one control unit, for row pitch.
constexpr float kUnitHeight = kReadoutDrop + kReadoutH * 0.5f;

//! Column pitch for a row of `n` units across `panelW` with `margin` a side.
//! Pass this to addControl as `cellW` so the unit can shrink to fit -- a fixed
//! kReadoutW of 72 px silently overlapped its neighbours the moment a row had
//! more than ~6 columns (a 34 HP panel with 8 columns gives only ~57 px each).
inline float pitch(float panelW, float margin, int n) {
    return n > 0 ? (panelW - 2.f * margin) / (float) n : panelW;
}

//! Lay out one control unit centred on (x, y), y being the KNOB centre.
//! `cellW` is the horizontal space this unit owns; 0 means "assume there is
//! room for the full-size unit".  When given, the readout and the knob are
//! clamped to fit inside it, so a crowded row degrades gracefully instead of
//! drawing controls on top of each other.
//! Height of one control unit at `scale`, knob top to readout bottom.  A module
//! with stacked rows MUST space them by at least this: the full-size unit is
//! ~98 px tall, so a panel packing rows at 76 px overlapped every one of them
//! with the next (that is what made VCO-4 look jumbled).
inline float unitHeight(float scale = 1.f, bool withReadout = true) {
    const float kr = std::max( 7.f, kKnobR    * scale);
    const float jr = std::max( 5.f, kJackR    * scale);
    const float gp = std::max( 5.f, kGap      * scale);
    const float rh = std::max(14.f, kReadoutH * scale);
    const float jd = kr + 2.f + gp + jr + 2.f;
    // Without a readout the unit ends at the jack, and the name sits ABOVE the
    // knob instead -- allow for that caption in the height.
    if (!withReadout) return 12.f + kr + 2.f + jd + jr + 2.f;
    return kr + 2.f + jd + jr + 2.f + gp + rh;
}

inline void addControl(PanelSpec& p, int valueId, int cvDepthId, int inputId,
                       int readoutId, float x, float y, const std::string& label,
                       float cellW = 0.f, float scale = 1.f) {
    // Floors, not pure scaling: a jack has to stay patchable with a mouse and a
    // readout has to stay tall enough for its glyphs, so shrinking a dense panel
    // trades knob size away first and stops before the unit becomes unusable.
    float knobR    = std::max( 7.f, kKnobR    * scale);
    float jackR    = std::max( 5.f, kJackR    * scale);
    float gap      = std::max( 5.f, kGap      * scale);
    float readoutH = std::max(14.f, kReadoutH * scale);
    float readoutW = kReadoutW * scale;
    if (cellW > 0.f) {
        // 6 px of clear panel between adjacent readouts, 10 px between knobs:
        // enough that the edges read as separate controls rather than a strip.
        readoutW = std::min(readoutW, std::max(28.f, cellW - 6.f));
        knobR    = std::min(knobR,    std::max( 7.f, (cellW - 10.f) * 0.5f));
    }

    // readoutId < 0 means "no segment display".  A panel with many stacked rows
    // buys back ~30 px per row that way, which is what lets the knobs and jacks
    // stay FULL SIZE instead of being shrunk to fit.  The name then goes ABOVE
    // the knob -- never between knob and jack, which reads as belonging to
    // neither (the original reason the readout sits under the jack at all).
    const bool withReadout = (readoutId >= 0);

    PanelElement k;
    k.id = valueId; k.x = x; k.y = y; k.radius = knobR;
    k.style = PanelControlStyle::KnobCV;
    k.cvParamId = cvDepthId;
    if (withReadout) {
        k.labelPlacement = PanelLabelPlacement::None; // the readout names it
    } else {
        k.label = label;
        k.labelPlacement = PanelLabelPlacement::Above;
    }
    p.params.push_back(k);

    // The drops are derived from the ACTUAL knob radius, not the nominal one:
    // a shrunk knob must pull its jack and readout up with it, or the 10 px of
    // clear panel the reading order depends on turns into a 20 px hole and the
    // unit stops looking like one control.
    const float jackDrop    = knobR + 2.f + gap + jackR + 2.f;
    const float readoutDrop = jackDrop + jackR + 2.f + gap + readoutH * 0.5f;

    PanelElement j;
    j.id = inputId; j.x = x; j.y = y + jackDrop; j.radius = jackR;
    j.style = PanelControlStyle::Knob;                // jack artwork
    j.labelPlacement = PanelLabelPlacement::None;
    p.inputs.push_back(j);

    if (!withReadout) return;

    PanelElement d;
    d.id = readoutId;                                 // inert; just a unique key
    d.cvParamId = valueId;                            // the param it reflects
    d.x = x; d.y = y + readoutDrop; d.radius = readoutH * 0.5f;
    d.width = readoutW; d.height = readoutH;
    d.style = PanelControlStyle::SegmentDisplay;
    d.label = label;                                  // shown at rest
    d.labelPlacement = PanelLabelPlacement::None;
    p.params.push_back(d);
}

//! A standalone segment display for a parameter that has no CV (a mode switch,
//! say): still shows its name at rest and its value while being turned.
inline void addReadout(PanelSpec& p, int valueId, float x, float y,
                       const std::string& label) {
    PanelElement d;
    d.id = valueId; d.x = x; d.y = y; d.radius = 11.f;
    d.width = kReadoutW; d.height = kReadoutH;
    d.style = PanelControlStyle::SegmentDisplay;
    d.label = label;
    d.labelPlacement = PanelLabelPlacement::None;
    p.params.push_back(d);
}

//! A captioned frame around a group of units (PanelSpec::decor).  Takes
//! TOP-LEFT; the editor draws Sections centred, which is a classic off-by-half.
inline PanelElement section(float x, float y, float w, float h,
                            const std::string& label) {
    PanelElement v;
    v.style = PanelControlStyle::Section;
    v.x = x + w * 0.5f; v.y = y + h * 0.5f;
    v.width = w; v.height = h; v.label = label;
    v.labelPlacement = PanelLabelPlacement::Above;
    return v;
}

//! Evenly spaced column centres inside a panel of width `panelW` with `margin`
//! either side.  Everything should be placed through this -- a hardcoded x
//! collides the moment a row's count changes.
inline float col(float panelW, float margin, int n, int i) {
    return margin + (panelW - 2.f * margin) / (float)n * ((float)i + 0.5f);
}

//! Configure the value + CV-depth params of one control unit.  `lo`/`hi` are the
//! value range; the ring is always a centred bipolar attenuverter named after
//! its parameter, so a readout can say "Cutoff CV" rather than "CV depth".
template <typename ModuleT>
inline void configControl(ModuleT& m, int valueId, int cvDepthId,
                          float lo, float hi, float def, const std::string& name,
                          const std::string& unit = "") {
    m.configParam(valueId, lo, hi, def, name, unit);
    m.configParam(cvDepthId, -1.f, 1.f, 0.f, name + " CV");
}

} // namespace kit
} // namespace rackx

#endif
