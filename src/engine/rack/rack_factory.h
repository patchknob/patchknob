//----------------------------------------------------------------------------
//  src/engine/rack/rack_factory.h
//
//  Module registry + factory.  Every built-in module type is described by a
//  ModuleType {slug,name,category,role,make}.  The generic SDL editor uses the
//  registry to populate its searchable "add module" palette, and RackEngine
//  uses make() to instantiate.  Registration is EXPLICIT (registerBuiltins())
//  rather than via file-scope static initializers, so nothing is stripped when
//  this is linked as a static library.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_RACK_FACTORY_H
#define PATCHKNOB_ENGINE_RACK_FACTORY_H

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rack.hpp"
#include "../sample_slot.h"

namespace rackx {

// Special roles let RackEngine wire a module to the outside world (the node's
// stereo audio + MIDI).  Normal modules only ever see cables.
enum class Role { Normal, AudioOut, AudioIn, MidiCV };

class ModuleHandle {
public:
    using Destroy = void (*)(rack::engine::Module*);

    ModuleHandle() = default;
    template <typename T, typename Deleter>
    ModuleHandle(std::unique_ptr<T, Deleter>&& module)
        : m_module(module.release()), m_destroy(&destroyNative) {}
    ModuleHandle(rack::engine::Module* module, Destroy destroy)
        : m_module(module), m_destroy(destroy ? destroy : &destroyNative) {}
    ~ModuleHandle() { reset(); }

    ModuleHandle(const ModuleHandle&) = delete;
    ModuleHandle& operator=(const ModuleHandle&) = delete;
    ModuleHandle(ModuleHandle&& other) noexcept
        : m_module(other.m_module), m_destroy(other.m_destroy) {
        other.m_module = nullptr;
    }
    ModuleHandle& operator=(ModuleHandle&& other) noexcept {
        if (this == &other) return *this;
        reset();
        m_module = other.m_module;
        m_destroy = other.m_destroy;
        other.m_module = nullptr;
        return *this;
    }

    rack::engine::Module* get() const { return m_module; }
    rack::engine::Module* operator->() const { return m_module; }
    explicit operator bool() const { return m_module != nullptr; }
    void reset() {
        if (m_module) m_destroy(m_module);
        m_module = nullptr;
        m_destroy = &destroyNative;
    }

private:
    static void destroyNative(rack::engine::Module* module) { delete module; }
    rack::engine::Module* m_module = nullptr;
    Destroy m_destroy = &destroyNative;
};

// Native, GPU-rendered panel geometry.  Coordinates are in Rack panel pixels
// before the editor zoom is applied, so translated modules retain their useful
// physical proportions without importing Cardinal's SVG widget layer.
// PianoKey / StepPad / Lamp exist for the acid-sequencer faceplate, which
// copies the original bassline box's layout: a one-octave entry keyboard over a
// grid of lit step pads.
//   PianoKey - a piano key.  Set `widget` to "black" for a sharp (drawn on top
//              of the naturals, shorter and narrower).  width/height size it.
//   StepPad  - a rectangular lit pad for a step grid; larger hit area and a
//              brighter lit state than Gate, with the label inside the pad.
//   Lamp     - a non-interactive round indicator driven by a light index.
//   Section  - a captioned frame used only as DECORATION (see PanelSpec::decor);
//              it groups the controls of one panel area the way the hardware's
//              silk-screened boxes do.  Never hit-tested.
//   SegmentDisplay - the little bezelled readout box (formerly "NumberBox").
//              Shows the PARAMETER NAME at rest and the SELECTED VALUE while the
//              knob is being turned, then reverts -- a hardware multi-switch
//              display.  Give it configSwitch() position names and it reads
//              "Ge OA90" instead of "1".
// APPEND-ONLY (saved panels store style by ordinal).
//   KnobCV - a CONCENTRIC pair: the inner disc is the base value (`id`), the
//            outer ring is the CV attenuverter for it (`cvParamId`).  One
//            control, two params, so a module can give every parameter its own
//            CV depth without tripling the panel width.  Hit-testing routes the
//            outer annulus to cvParamId and the inner disc to id, which means
//            the existing drag / right-click-reset paths need no changes.
enum class PanelControlStyle { Knob, Slider, Switch, Button, Gate, SegmentDisplay,
                               PianoKey, StepPad, Lamp, Section, Curve, Waveform,
                               KnobCV };
// Waveform: an inline sample display for a module implementing
// PatchKnob::engine::ISampleSlot.  Like Curve it needs a UNIQUE real param
// id (find_element keys on id) and uses width/height for its box; it draws
// the slot's peaks plus its start/end/loop markers.

//----------------------------------------------------------------------------
//  Breakpoint-curve editing as a REUSABLE panel widget.
//
//  A single float param cannot express a curve, so a module that wants the
//  graphical editor implements ICurveSource and the panel talks to it directly.
//  A PanelElement with style == Curve binds to CURVE INDEX `id` (not a param
//  id) and uses width/height for its box instead of radius.
//
//  Deliberately generic: anything with draggable breakpoints over a normalised
//  span -- envelopes, automation lanes, wavetable shapers, velocity curves --
//  can implement this and get the same editor.
//----------------------------------------------------------------------------
struct CurvePoint {
    float t = 0.f;    //!< normalised position across the span, 0..1
    float v = 0.f;    //!< normalised value, 0..1
    float c = 0.f;    //!< exponential curve toward the NEXT point, -1..1
};

struct CurveInfo {
    float loopStart = 0.f, loopEnd = 1.f;
    bool  loopOn    = false;
    float gateStart = 0.f, gateEnd = 1.f;
    float gridStep  = 0.f;    //!< snap step as a fraction of the span; 0 == off
    float playhead  = -1.f;   //!< 0..1 while running, negative when idle
    int   selected  = -1;
    float spanBars    = 0.f;  //!< musical length, for the step-number ruler
    int   beatsPerBar = 4;
};

class ICurveSource {
public:
    virtual ~ICurveSource() {}
    virtual int   curveCount() const = 0;
    virtual int   curvePointCount( int curve ) const = 0;
    virtual bool  curveGetPoint( int curve, int index, CurvePoint& out ) const = 0;
    virtual bool  curveSetPoint( int curve, int index, const CurvePoint& p ) = 0;
    //! Insert a point; returns its index, or -1 when the curve is full.
    virtual int   curveAddPoint( int curve, float t, float v ) = 0;
    virtual bool  curveRemovePoint( int curve, int index ) = 0;
    virtual void  curveGetInfo( int curve, CurveInfo& out ) const = 0;
    virtual void  curveSetSelected( int curve, int index ) { (void)curve; (void)index; }
    //! Snap a normalised time to this curve's musical grid (identity when off).
    virtual float curveSnapTime( int curve, float t ) const { (void)curve; return t; }
    //! Value of the curve at normalised position t (so the editor draws the
    //! SAME shape the DSP plays, rather than re-deriving the interpolation).
    virtual float curveValueAt( int curve, float t ) const = 0;
    //! Bend segment `seg` (between point seg and seg+1) so its MIDPOINT passes
    //! through `midValue` (0..1).  This is how the editor turns a vertical drag
    //! of a segment handle into a curve amount without knowing the module's
    //! interpolation law.  Default: no bending available.
    virtual void curveSetSegmentMid( int curve, int seg, float midValue )
    { (void)curve; (void)seg; (void)midValue; }
};
// Sample-holding modules implement PatchKnob::engine::ISampleSlot (see
// engine/sample_slot.h): the SAME contract the Buzz sampler instrument
// implements, so one editor + one disk browser serve both.
using ISampleSlot = PatchKnob::engine::ISampleSlot;

enum class PanelLabelPlacement { None, Above, Below, Left, Right };

constexpr float RACK_HP_WIDTH = 15.24f;
constexpr float RACK_PANEL_HEIGHT = 380.f;

struct PanelElement {
    int               id = -1;
    float             x = 0.f;
    float             y = 0.f;
    float             radius = 0.f;
    PanelControlStyle style = PanelControlStyle::Knob;
    std::string       label;
    PanelLabelPlacement labelPlacement = PanelLabelPlacement::Below;
    float             width = 0.f;
    float             height = 0.f;
    std::string       widget;
    // Tab page this element belongs to (see PanelSpec::tabs).  -1 == always
    // visible (drawn on every tab -- e.g. shared I/O jacks).
    int               tab = -1;
    // For PanelControlStyle::Curve only: which ICurveSource curve this widget
    // edits.  `id` still has to be a UNIQUE, REAL param id (find_element()
    // matches elements by id, so reusing a curve index there would collide with
    // the param of the same number); this is the separate curve selector.
    int               curveIndex = -1;
    // For PanelControlStyle::KnobCV: the param the OUTER ring edits (the CV
    // attenuverter).  `id` stays the inner/base param, so every other code path
    // that keys on `id` keeps working.
    //
    // For PanelControlStyle::SegmentDisplay: the param this display REFLECTS.
    // find_element() keys on `id` and returns the first match, so a param can
    // only own one panel element -- the knob already owns it.  The display
    // therefore carries its own (inert) `id` and points here at the knob it
    // belongs to, showing that parameter's name at rest and its value while it
    // is being turned.
    int               cvParamId = -1;
};

struct PanelSpec {
    float width = 0.f;
    float height = 0.f;
    float headerHeight = 20.f;
    std::string textureAsset;
    std::string texturePack;
    std::vector<PanelElement> params;
    std::vector<PanelElement> inputs;
    std::vector<PanelElement> outputs;
    std::vector<PanelElement> lights;
    // Non-interactive artwork drawn BEFORE every control, so a Section frame
    // sits behind the knobs it groups.  Entries carry no param binding; `id` is
    // ignored and they are never hit-tested.
    std::vector<PanelElement> decor;
    // Optional tab pages.  When non-empty the editor draws a tab bar under the
    // title and shows only elements whose PanelElement::tab matches the active
    // tab (plus tab == -1 elements, shown on every tab).
    std::vector<std::string> tabs;

    bool valid() const { return width > 0.f && height > 0.f; }
    static PanelSpec fromHp(int hp) {
        PanelSpec panel;
        panel.width = hp > 0 ? hp * RACK_HP_WIDTH : RACK_HP_WIDTH;
        panel.height = RACK_PANEL_HEIGHT;
        return panel;
    }
};

struct ModuleType {
    std::string slug;        // stable id, e.g. "VCO"
    std::string name;        // display name, e.g. "VCO-1"
    std::string category;    // "Oscillator" / "Filter" / "Envelope" / "I/O" ...
    Role        role = Role::Normal;
    std::function<ModuleHandle()> make;
    PanelSpec   panel;
    bool        paletteVisible = true;
};

// The global registry (populated once by registerBuiltins()).
std::vector<ModuleType>& registry();
void                      registerBuiltins();     // idempotent
// Registers the scripting modules (Pd / Csound).  Defined in PatchKnob_patch (it
// needs libpd + Csound); calls registerBuiltins() first, so it is a safe superset.
void                      registerRackExtModules();
const ModuleType*         findType(const std::string& slug);
ModuleHandle               createModule(const std::string& slug);
Role                      roleOf(const std::string& slug);

// Helper used by rack_modules.cpp to add one entry.
inline void addType(std::string slug, std::string name, std::string category, Role role,
                    std::function<ModuleHandle()> make,
                    PanelSpec panel = {}, bool paletteVisible = true) {
    registry().push_back(ModuleType{ std::move(slug), std::move(name), std::move(category),
                                     role, std::move(make), std::move(panel), paletteVisible });
}

} // namespace rackx

#endif
