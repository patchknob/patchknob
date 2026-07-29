//----------------------------------------------------------------------------
//  src/engine/rack/rack_widgets_compat.hpp
//
//  Inert widget/app/componentlibrary API for Rack SDK bridge builds.  Plugin
//  headers define custom widget subclasses (knobs, ports, displays) next to
//  their DSP; the bridge compiles them as dead code -- the SDL renderer draws
//  panels from pre-rendered textures and manifest layouts instead.  Nothing
//  here is ever instantiated by the bridge.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_RACK_WIDGETS_COMPAT_HPP
#define PATCHKNOB_ENGINE_RACK_WIDGETS_COMPAT_HPP

#include "rack.hpp"
#include "nanovg.h"

#include <memory>
#include <string>
#include <vector>

struct NSVGimage {
    float width = 0.f;
    float height = 0.f;
    void* shapes = nullptr;
};

namespace rack {

namespace color {
static const NVGcolor BLACK_TRANSPARENT = nvgRGBA(0x00, 0x00, 0x00, 0x00);
static const NVGcolor WHITE_TRANSPARENT = nvgRGBA(0xff, 0xff, 0xff, 0x00);
static const NVGcolor BLACK = nvgRGB(0x00, 0x00, 0x00);
static const NVGcolor WHITE = nvgRGB(0xff, 0xff, 0xff);
static const NVGcolor RED = nvgRGB(0xff, 0x00, 0x00);
static const NVGcolor GREEN = nvgRGB(0x00, 0xff, 0x00);
static const NVGcolor BLUE = nvgRGB(0x00, 0x00, 0xff);
static const NVGcolor CYAN = nvgRGB(0x00, 0xff, 0xff);
static const NVGcolor MAGENTA = nvgRGB(0xff, 0x00, 0xff);
static const NVGcolor YELLOW = nvgRGB(0xff, 0xff, 0x00);
inline NVGcolor fromHexString(const std::string&) { return BLACK; }
inline std::string toHexString(NVGcolor) { return "#000000"; }
inline NVGcolor mult(NVGcolor c, float x) {
    for (int i = 0; i < 3; ++i) c.rgba[i] *= x;
    return c;
}
inline NVGcolor mult(NVGcolor c, NVGcolor d) {
    for (int i = 0; i < 3; ++i) c.rgba[i] *= d.rgba[i];
    return c;
}
inline NVGcolor screen(NVGcolor c, NVGcolor) { return c; }
inline NVGcolor alpha(NVGcolor c, float a) { c.a *= a; return c; }
inline NVGcolor plus(NVGcolor c, NVGcolor d) {
    for (int i = 0; i < 3; ++i) c.rgba[i] += d.rgba[i];
    return c;
}
inline NVGcolor lerp(NVGcolor c, NVGcolor d, float u) { return nvgLerpRGBA(c, d, u); }
inline bool isEqual(NVGcolor c, NVGcolor d) {
    for (int i = 0; i < 4; ++i) if (c.rgba[i] != d.rgba[i]) return false;
    return true;
}
}

// Rack component-library color scheme constants.
static const NVGcolor SCHEME_BLACK_TRANSPARENT = nvgRGBA(0x00, 0x00, 0x00, 0x00);
static const NVGcolor SCHEME_BLACK = nvgRGB(0x00, 0x00, 0x00);
static const NVGcolor SCHEME_WHITE = nvgRGB(0xff, 0xff, 0xff);
static const NVGcolor SCHEME_RED = nvgRGB(0xed, 0x2c, 0x24);
static const NVGcolor SCHEME_ORANGE = nvgRGB(0xf2, 0xb1, 0x20);
static const NVGcolor SCHEME_YELLOW = nvgRGB(0xff, 0xd7, 0x14);
static const NVGcolor SCHEME_GREEN = nvgRGB(0x90, 0xc7, 0x3e);
static const NVGcolor SCHEME_CYAN = nvgRGB(0x22, 0xe6, 0xef);
static const NVGcolor SCHEME_BLUE = nvgRGB(0x29, 0xb2, 0xef);
static const NVGcolor SCHEME_PURPLE = nvgRGB(0xd5, 0x2b, 0xed);
static const NVGcolor SCHEME_LIGHT_GRAY = nvgRGB(0xe6, 0xe6, 0xe6);
static const NVGcolor SCHEME_GRAY = nvgRGB(0x80, 0x80, 0x80);
static const NVGcolor SCHEME_DARK_GRAY = nvgRGB(0x17, 0x17, 0x17);

namespace window {
struct Svg {
    NSVGimage* handle = nullptr;
    void loadFile(const std::string&) {}
    void loadString(const std::string&) {}
    math::Vec getSize() const { return math::Vec(0.f, 0.f); }
    static std::shared_ptr<Svg> load(const std::string&) { return std::make_shared<Svg>(); }
};
struct Font {
    int handle = -1;
    static std::shared_ptr<Font> load(const std::string&) { return std::make_shared<Font>(); }
};
struct Image {
    int handle = -1;
    static std::shared_ptr<Image> load(const std::string&) { return std::make_shared<Image>(); }
};
struct Window {
    NVGcontext* vg = nullptr;
    void* win = nullptr;     // GLFWwindow* in real Rack
    float pixelRatio = 1.f;
    std::shared_ptr<Font> uiFont = std::make_shared<Font>();
    std::shared_ptr<Svg> loadSvg(const std::string&) { return std::make_shared<Svg>(); }
    std::shared_ptr<Font> loadFont(const std::string&) { return std::make_shared<Font>(); }
    std::shared_ptr<Image> loadImage(const std::string&) { return std::make_shared<Image>(); }
};
inline Window& windowInstance() { static Window value; return value; }
inline void svgDraw(NVGcontext*, NSVGimage*) {}
} // namespace window
using window::Svg;
using window::Font;
using window::Image;
using window::svgDraw;
typedef Svg SVG;   // v1 alias

namespace history {
struct Action {
    std::string name;
    virtual ~Action() = default;
    virtual void undo() {}
    virtual void redo() {}
};
struct ComplexAction : Action {
    std::vector<Action*> actions;
    ~ComplexAction() override { for (Action* action : actions) delete action; }
    void push(Action* action) { actions.push_back(action); }
    bool isEmpty() const { return actions.empty(); }
};
struct ModuleAction : Action { int64_t moduleId = -1; };
struct ModuleAdd : ModuleAction {};
struct ModuleMove : ModuleAction { math::Vec oldPos, newPos; };
struct ModuleBypass : ModuleAction { bool bypassed = false; };
struct ModuleChange : ModuleAction {};
struct ParamChange : ModuleAction { int paramId = -1; float oldValue = 0.f, newValue = 0.f; };
struct CableAdd : Action {};
struct CableRemove : Action {};
struct State {
    void push(Action* action) { delete action; }
    void undo() {}
    void redo() {}
    bool canUndo() const { return false; }
    bool canRedo() const { return false; }
    void setSaved() {}
};
inline State& stateInstance() { static State value; return value; }
} // namespace history

// bind APP->window / APP->history for widget code compiled into bridge DLLs
inline const bool s24_windowBound =
    (contextGet()->window = &window::windowInstance(),
     contextGet()->history = &history::stateInstance(), true);

namespace engine { struct Module; }

namespace widget {

struct BaseEvent {
    void stopPropagating() const {}
    void consume(void*) const {}
    void unconsume() const {}
    bool isConsumed() const { return false; }
    void* getTarget() const { return nullptr; }
};

struct Widget {
    math::Rect box = math::Rect(math::Vec(0.f, 0.f), math::Vec(0.f, 0.f));
    Widget* parent = nullptr;
    std::vector<Widget*> children;
    bool visible = true;
    bool requestedDelete = false;

    struct DrawArgs {
        NVGcontext* vg = nullptr;
        math::Rect clipBox = math::Rect(math::Vec(0.f, 0.f), math::Vec(0.f, 0.f));
        void* fb = nullptr;
    };

    // event aliases used by `EventType e` / `const HoverEvent&` signatures
    struct PositionEvent : BaseEvent { math::Vec pos; };
    struct HoverEvent : PositionEvent { math::Vec mouseDelta; };
    struct ButtonEvent : PositionEvent { int button = 0; int action = 0; int mods = 0; };
    struct DoubleClickEvent : BaseEvent {};
    struct HoverKeyEvent : PositionEvent { int key = 0; int scancode = 0; int action = 0; int mods = 0; std::string keyName; };
    struct HoverTextEvent : PositionEvent { int codepoint = 0; };
    struct HoverScrollEvent : PositionEvent { math::Vec scrollDelta; };
    struct EnterEvent : BaseEvent {};
    struct LeaveEvent : BaseEvent {};
    struct SelectEvent : BaseEvent {};
    struct DeselectEvent : BaseEvent {};
    struct SelectKeyEvent : BaseEvent { int key = 0; int scancode = 0; int action = 0; int mods = 0; std::string keyName; };
    struct SelectTextEvent : BaseEvent { int codepoint = 0; };
    struct DragBaseEvent : BaseEvent { int button = 0; };
    struct DragStartEvent : DragBaseEvent {};
    struct DragEndEvent : DragBaseEvent {};
    struct DragMoveEvent : DragBaseEvent { math::Vec mouseDelta; };
    struct DragHoverEvent : DragBaseEvent { math::Vec pos; math::Vec mouseDelta; Widget* origin = nullptr; };
    struct DragEnterEvent : DragBaseEvent { Widget* origin = nullptr; };
    struct DragLeaveEvent : DragBaseEvent { Widget* origin = nullptr; };
    struct DragDropEvent : DragBaseEvent { Widget* origin = nullptr; };
    struct PathDropEvent : PositionEvent { std::vector<std::string> paths; };
    struct ActionEvent : BaseEvent {};
    struct ChangeEvent : BaseEvent {};
    struct DirtyEvent : BaseEvent {};
    struct RepositionEvent : BaseEvent {};
    struct ResizeEvent : BaseEvent {};
    struct AddEvent : BaseEvent {};
    struct RemoveEvent : BaseEvent {};
    struct ShowEvent : BaseEvent {};
    struct HideEvent : BaseEvent {};
    struct ContextCreateEvent : BaseEvent { NVGcontext* vg = nullptr; };
    struct ContextDestroyEvent : BaseEvent { NVGcontext* vg = nullptr; };

    virtual ~Widget() {
        for (Widget* child : children) delete child;
    }
    math::Rect getBox() const { return box; }
    void setBox(math::Rect b) { box = b; }
    math::Vec getPosition() const { return box.pos; }
    void setPosition(math::Vec position) { box.pos = position; }
    math::Vec getSize() const { return box.size; }
    void setSize(math::Vec size) { box.size = size; }
    bool isVisible() const { return visible; }
    void setVisible(bool value) { visible = value; }
    void show() { visible = true; }
    void hide() { visible = false; }
    void requestDelete() { requestedDelete = true; }
    void addChild(Widget* child) { if (child) { child->parent = this; children.push_back(child); } }
    void addChildBottom(Widget* child) { if (child) { child->parent = this; children.insert(children.begin(), child); } }
    void addChildBelow(Widget* child, Widget*) { addChild(child); }
    void addChildAbove(Widget* child, Widget*) { addChild(child); }
    void removeChild(Widget*) {}
    void clearChildren() { children.clear(); }
    virtual void step() {}
    virtual void draw(const DrawArgs&) {}
    virtual void drawLayer(const DrawArgs&, int) {}
    virtual void onHover(const HoverEvent&) {}
    virtual void onButton(const ButtonEvent&) {}
    virtual void onDoubleClick(const DoubleClickEvent&) {}
    virtual void onHoverKey(const HoverKeyEvent&) {}
    virtual void onHoverText(const HoverTextEvent&) {}
    virtual void onHoverScroll(const HoverScrollEvent&) {}
    virtual void onEnter(const EnterEvent&) {}
    virtual void onLeave(const LeaveEvent&) {}
    virtual void onSelect(const SelectEvent&) {}
    virtual void onDeselect(const DeselectEvent&) {}
    virtual void onSelectKey(const SelectKeyEvent&) {}
    virtual void onSelectText(const SelectTextEvent&) {}
    virtual void onDragStart(const DragStartEvent&) {}
    virtual void onDragEnd(const DragEndEvent&) {}
    virtual void onDragMove(const DragMoveEvent&) {}
    virtual void onDragHover(const DragHoverEvent&) {}
    virtual void onDragEnter(const DragEnterEvent&) {}
    virtual void onDragLeave(const DragLeaveEvent&) {}
    virtual void onDragDrop(const DragDropEvent&) {}
    virtual void onPathDrop(const PathDropEvent&) {}
    virtual void onAction(const ActionEvent&) {}
    virtual void onChange(const ChangeEvent&) {}
    virtual void onDirty(const DirtyEvent&) {}
    virtual void onReposition(const RepositionEvent&) {}
    virtual void onResize(const ResizeEvent&) {}
    virtual void onAdd(const AddEvent&) {}
    virtual void onRemove(const RemoveEvent&) {}
    virtual void onShow(const ShowEvent&) {}
    virtual void onHide(const HideEvent&) {}
    virtual void onContextCreate(const ContextCreateEvent&) {}
    virtual void onContextDestroy(const ContextDestroyEvent&) {}
};

struct TransparentWidget : Widget {};
struct OpaqueWidget : Widget {};

struct SvgWidget : Widget {
    std::shared_ptr<Svg> svg;
    void wrap() {}
    void setSvg(std::shared_ptr<Svg> s) { svg = s; }
    void setSVG(std::shared_ptr<Svg> s) { svg = s; }
};

struct FramebufferWidget : Widget {
    bool dirty = true;
    float oversample = 1.f;
    void setDirty(bool value = true) { dirty = value; }
};

struct TransformWidget : Widget {
    void identity() {}
    void translate(math::Vec) {}
    void rotate(float) {}
    void rotate(float, math::Vec) {}
    void scale(math::Vec) {}
};

struct ZoomWidget : Widget {
    void setZoom(float) {}
};

} // namespace widget

// v1 event namespace aliases (plugins with legacy signatures)
namespace event {
typedef widget::Widget::HoverEvent Hover;
typedef widget::Widget::ButtonEvent Button;
typedef widget::Widget::DoubleClickEvent DoubleClick;
typedef widget::Widget::HoverKeyEvent HoverKey;
typedef widget::Widget::HoverScrollEvent HoverScroll;
typedef widget::Widget::EnterEvent Enter;
typedef widget::Widget::LeaveEvent Leave;
typedef widget::Widget::SelectEvent Select;
typedef widget::Widget::DeselectEvent Deselect;
typedef widget::Widget::SelectKeyEvent SelectKey;
typedef widget::Widget::SelectTextEvent SelectText;
typedef widget::Widget::DragStartEvent DragStart;
typedef widget::Widget::DragEndEvent DragEnd;
typedef widget::Widget::DragMoveEvent DragMove;
typedef widget::Widget::DragHoverEvent DragHover;
typedef widget::Widget::DragEnterEvent DragEnter;
typedef widget::Widget::DragLeaveEvent DragLeave;
typedef widget::Widget::DragDropEvent DragDrop;
typedef widget::Widget::ActionEvent Action;
typedef widget::Widget::ChangeEvent Change;
}

namespace ui {
struct Menu : widget::OpaqueWidget {
    void addChild(widget::Widget* child) { widget::OpaqueWidget::addChild(child); }
};
struct MenuEntry : widget::OpaqueWidget {};
struct MenuSeparator : MenuEntry {};
struct MenuLabel : MenuEntry { std::string text; };
struct MenuItem : MenuEntry {
    std::string text;
    std::string rightText;
    bool disabled = false;
    virtual Menu* createChildMenu() { return nullptr; }
};
struct Button : widget::OpaqueWidget { std::string text; };
struct ChoiceButton : Button {};
struct Label : widget::Widget {
    std::string text;
    float fontSize = 13.f;
    NVGcolor color = color::BLACK;
    int alignment = 0;
};
struct Tooltip : widget::Widget { std::string text; };
struct Slider : widget::OpaqueWidget { void* quantity = nullptr; };
struct TextField : widget::OpaqueWidget {
    std::string text;
    std::string placeholder;
    bool multiline = false;
    std::string getText() const { return text; }
    void setText(std::string value) { text = std::move(value); }
    void selectAll() {}
};
struct SequentialLayout : widget::Widget {
    enum Orientation { HORIZONTAL_ORIENTATION, VERTICAL_ORIENTATION };
    Orientation orientation = HORIZONTAL_ORIENTATION;
    math::Vec spacing;
};
}

namespace app {

static const float RACK_GRID_WIDTH_PX = 15.f;

struct CircularShadow : widget::TransparentWidget {
    float blurRadius = 0.f;
    float opacity = 0.15f;
};

struct MultiLightWidget : widget::TransparentWidget {
    std::vector<NVGcolor> baseColors;
    NVGcolor bgColor = color::BLACK_TRANSPARENT;
    NVGcolor color = color::BLACK_TRANSPARENT;
    NVGcolor borderColor = color::BLACK_TRANSPARENT;
    void addBaseColor(NVGcolor c) { baseColors.push_back(c); }
};
typedef MultiLightWidget LightWidget;

struct ModuleLightWidget : MultiLightWidget {
    engine::Module* module = nullptr;
    int firstLightId = -1;
};

struct ParamWidget : widget::OpaqueWidget {
    engine::Module* module = nullptr;
    int paramId = -1;
    engine::ParamQuantity* getParamQuantity() {
        return module ? module->getParamQuantity(paramId) : nullptr;
    }
    virtual void initParamQuantity() {}
    void createContextMenu() {}
    virtual void appendContextMenu(ui::Menu*) {}
    void resetAction() {}
};

struct Knob : ParamWidget {
    bool horizontal = false;
    bool smooth = true;
    bool snap = false;
    float speed = 1.f;
    bool forceLinear = false;
    float minAngle = -float(M_PI) * 0.75f;
    float maxAngle = float(M_PI) * 0.75f;
};

struct SvgKnob : Knob {
    widget::FramebufferWidget* fb = nullptr;
    CircularShadow* shadow = nullptr;
    widget::TransformWidget* tw = nullptr;
    widget::SvgWidget* sw = nullptr;
    SvgKnob() {
        fb = new widget::FramebufferWidget;
        shadow = new CircularShadow;
        tw = new widget::TransformWidget;
        sw = new widget::SvgWidget;
        addChild(fb);
        fb->addChild(shadow);
        fb->addChild(tw);
        tw->addChild(sw);
    }
    void setSvg(std::shared_ptr<Svg> svg) { sw->setSvg(svg); }
    void setSVG(std::shared_ptr<Svg> svg) { setSvg(svg); }
};
typedef SvgKnob SVGKnob;

struct SliderKnob : Knob {};

struct SvgSlider : SliderKnob {
    widget::FramebufferWidget* fb = nullptr;
    widget::SvgWidget* background = nullptr;
    widget::SvgWidget* handle = nullptr;
    math::Vec minHandlePos, maxHandlePos;
    SvgSlider() {
        fb = new widget::FramebufferWidget;
        background = new widget::SvgWidget;
        handle = new widget::SvgWidget;
        addChild(fb);
        fb->addChild(background);
        fb->addChild(handle);
    }
    void setBackgroundSvg(std::shared_ptr<Svg> svg) { background->setSvg(svg); }
    void setHandleSvg(std::shared_ptr<Svg> svg) { handle->setSvg(svg); }
    void setHandlePos(math::Vec minPos, math::Vec maxPos) { minHandlePos = minPos; maxHandlePos = maxPos; }
    void setBackgroundSVG(std::shared_ptr<Svg> svg) { setBackgroundSvg(svg); }
    void setHandleSVG(std::shared_ptr<Svg> svg) { setHandleSvg(svg); }
    void setHandlePosCentered(math::Vec, math::Vec) {}
};
typedef SvgSlider SVGSlider;

struct Switch : ParamWidget {
    bool momentary = false;
};

struct SvgSwitch : Switch {
    widget::FramebufferWidget* fb = nullptr;
    CircularShadow* shadow = nullptr;
    widget::SvgWidget* sw = nullptr;
    std::vector<std::shared_ptr<Svg>> frames;
    bool latch = false;
    SvgSwitch() {
        fb = new widget::FramebufferWidget;
        shadow = new CircularShadow;
        sw = new widget::SvgWidget;
        addChild(fb);
        fb->addChild(shadow);
        fb->addChild(sw);
    }
    void addFrame(std::shared_ptr<Svg> svg) { frames.push_back(svg); }
};
typedef SvgSwitch SVGSwitch;

struct SvgButton : widget::OpaqueWidget {
    widget::FramebufferWidget* fb = nullptr;
    CircularShadow* shadow = nullptr;
    widget::SvgWidget* sw = nullptr;
    std::vector<std::shared_ptr<Svg>> frames;
    SvgButton() {
        fb = new widget::FramebufferWidget;
        shadow = new CircularShadow;
        sw = new widget::SvgWidget;
        addChild(fb);
        fb->addChild(shadow);
        fb->addChild(sw);
    }
    void addFrame(std::shared_ptr<Svg> svg) { frames.push_back(svg); }
};
typedef SvgButton SVGButton;

struct PortWidget : widget::OpaqueWidget {
    engine::Module* module = nullptr;
    engine::Port::Type type = engine::Port::INPUT;
    int portId = -1;
    void createContextMenu() {}
    virtual void appendContextMenu(ui::Menu*) {}
};

struct SvgPort : PortWidget {
    widget::FramebufferWidget* fb = nullptr;
    CircularShadow* shadow = nullptr;
    widget::SvgWidget* sw = nullptr;
    SvgPort() {
        fb = new widget::FramebufferWidget;
        shadow = new CircularShadow;
        sw = new widget::SvgWidget;
        addChild(fb);
        fb->addChild(shadow);
        fb->addChild(sw);
    }
    void setSvg(std::shared_ptr<Svg> svg) { sw->setSvg(svg); }
    void setSVG(std::shared_ptr<Svg> svg) { setSvg(svg); }
};
typedef SvgPort SVGPort;

struct SvgScrew : widget::Widget {
    widget::FramebufferWidget* fb = nullptr;
    widget::SvgWidget* sw = nullptr;
    SvgScrew() {
        fb = new widget::FramebufferWidget;
        sw = new widget::SvgWidget;
        addChild(fb);
        fb->addChild(sw);
    }
    void setSvg(std::shared_ptr<Svg> svg) { sw->setSvg(svg); }
    void setSVG(std::shared_ptr<Svg> svg) { setSvg(svg); }
};
typedef SvgScrew SVGScrew;

struct SvgPanel : widget::Widget {
    widget::FramebufferWidget* fb = nullptr;
    widget::SvgWidget* sw = nullptr;
    std::shared_ptr<Svg> svg;
    void setBackground(std::shared_ptr<Svg> s) { svg = s; }
};

struct ModuleWidget : widget::OpaqueWidget {
    engine::Module* module = nullptr;
    widget::Widget* panel = nullptr;
    engine::Module* getModule() { return module; }
    void setModule(engine::Module* m) { module = m; }
    void setPanel(widget::Widget* p) { panel = p; if (p) addChild(p); }
    void setPanel(std::shared_ptr<Svg>) {}
    void addParam(ParamWidget* w) { addChild(w); }
    void addInput(PortWidget* w) { addChild(w); }
    void addOutput(PortWidget* w) { addChild(w); }
    ParamWidget* getParam(int) { return nullptr; }
    PortWidget* getInput(int) { return nullptr; }
    PortWidget* getOutput(int) { return nullptr; }
    virtual void appendContextMenu(ui::Menu*) {}
    void createContextMenu() {}
};

struct RackScrollWidget : widget::Widget {};
struct RackWidget : widget::Widget {};
struct Scene : widget::Widget { RackScrollWidget* rackScroll = nullptr; RackWidget* rack = nullptr; };

struct LedDisplay : widget::Widget {};
struct LedDisplaySeparator : widget::TransparentWidget {};
struct LedDisplayChoice : widget::TransparentWidget {
    std::string text;
    std::shared_ptr<Font> font;
    std::string fontPath;
    math::Vec textOffset;
    NVGcolor color = color::WHITE;
    NVGcolor bgColor = color::BLACK_TRANSPARENT;
};
struct LedDisplayTextField : ui::TextField {
    std::shared_ptr<Font> font;
    std::string fontPath;
    math::Vec textOffset;
    NVGcolor color = color::WHITE;
    NVGcolor bgColor = color::BLACK_TRANSPARENT;
};

} // namespace app

using app::CircularShadow;
using app::LightWidget;
using app::MultiLightWidget;
using app::ModuleLightWidget;
using app::ParamWidget;
using app::Knob;
using app::SvgKnob;
using app::SVGKnob;
using app::SliderKnob;
using app::SvgSlider;
using app::SVGSlider;
using app::Switch;
using app::SvgSwitch;
using app::SVGSwitch;
using app::SvgButton;
using app::SVGButton;
using app::PortWidget;
using app::SvgPort;
using app::SVGPort;
using app::SvgScrew;
using app::SVGScrew;
using app::SvgPanel;
using app::ModuleWidget;
using app::LedDisplay;
using app::LedDisplaySeparator;
using app::LedDisplayChoice;
using app::LedDisplayTextField;
using widget::Widget;
using widget::TransparentWidget;
using widget::OpaqueWidget;
using widget::SvgWidget;
using widget::FramebufferWidget;
using widget::TransformWidget;
using widget::ZoomWidget;
using ui::Menu;
using ui::MenuEntry;
using ui::MenuSeparator;
using ui::MenuLabel;
using ui::MenuItem;
using ui::Button;
using ui::ChoiceButton;
using ui::Label;
using ui::Tooltip;
using ui::Slider;
using ui::TextField;
using ui::SequentialLayout;

// ---- component library (inert) ---------------------------------------------

namespace componentlibrary {

template <typename TBase = ModuleLightWidget>
struct TSvgLight : TBase {
    widget::FramebufferWidget* fb = nullptr;
    widget::SvgWidget* sw = nullptr;
    TSvgLight() {
        fb = new widget::FramebufferWidget;
        sw = new widget::SvgWidget;
        this->addChild(fb);
        fb->addChild(sw);
    }
    void setSvg(std::shared_ptr<Svg> svg) { sw->setSvg(svg); }
};
typedef TSvgLight<> SvgLight;

template <typename TBase = ModuleLightWidget>
struct TGrayModuleLightWidget : TBase {};
typedef TGrayModuleLightWidget<> GrayModuleLightWidget;

template <typename TBase = GrayModuleLightWidget>
struct TWhiteLight : TBase { TWhiteLight() { this->addBaseColor(SCHEME_WHITE); } };
typedef TWhiteLight<> WhiteLight;
template <typename TBase = GrayModuleLightWidget>
struct TRedLight : TBase { TRedLight() { this->addBaseColor(SCHEME_RED); } };
typedef TRedLight<> RedLight;
template <typename TBase = GrayModuleLightWidget>
struct TGreenLight : TBase { TGreenLight() { this->addBaseColor(SCHEME_GREEN); } };
typedef TGreenLight<> GreenLight;
template <typename TBase = GrayModuleLightWidget>
struct TBlueLight : TBase { TBlueLight() { this->addBaseColor(SCHEME_BLUE); } };
typedef TBlueLight<> BlueLight;
template <typename TBase = GrayModuleLightWidget>
struct TYellowLight : TBase { TYellowLight() { this->addBaseColor(SCHEME_YELLOW); } };
typedef TYellowLight<> YellowLight;
template <typename TBase = GrayModuleLightWidget>
struct TOrangeLight : TBase { TOrangeLight() { this->addBaseColor(SCHEME_ORANGE); } };
typedef TOrangeLight<> OrangeLight;
template <typename TBase = GrayModuleLightWidget>
struct TPurpleLight : TBase { TPurpleLight() { this->addBaseColor(SCHEME_PURPLE); } };
typedef TPurpleLight<> PurpleLight;
template <typename TBase = GrayModuleLightWidget>
struct TCyanLight : TBase { TCyanLight() { this->addBaseColor(SCHEME_CYAN); } };
typedef TCyanLight<> CyanLight;
template <typename TBase = GrayModuleLightWidget>
struct TGreenRedLight : TBase {
    TGreenRedLight() { this->addBaseColor(SCHEME_GREEN); this->addBaseColor(SCHEME_RED); }
};
typedef TGreenRedLight<> GreenRedLight;
template <typename TBase = GrayModuleLightWidget>
struct TRedGreenBlueLight : TBase {
    TRedGreenBlueLight() {
        this->addBaseColor(SCHEME_RED);
        this->addBaseColor(SCHEME_GREEN);
        this->addBaseColor(SCHEME_BLUE);
    }
};
typedef TRedGreenBlueLight<> RedGreenBlueLight;

template <typename TBase>
struct LargeLight : TSvgLight<TBase> {};
template <typename TBase>
struct MediumLight : TSvgLight<TBase> {};
template <typename TBase>
struct SmallLight : TSvgLight<TBase> {};
template <typename TBase>
struct TinyLight : TSvgLight<TBase> {};
template <typename TBase = GrayModuleLightWidget>
struct LargeSimpleLight : TBase {};
template <typename TBase = GrayModuleLightWidget>
struct MediumSimpleLight : TBase {};
template <typename TBase = GrayModuleLightWidget>
struct SmallSimpleLight : TBase {};
template <typename TBase = GrayModuleLightWidget>
struct TinySimpleLight : TBase {};
template <typename TBase>
struct RectangleLight : TBase {};
template <typename TBase>
struct VCVBezelLight : TBase {};
template <typename TBase>
struct LEDBezelLight : TBase {};
template <typename TBase>
struct PB61303Light : TBase {};

struct RoundKnob : SvgKnob {};
struct RoundBlackKnob : RoundKnob {};
struct RoundSmallBlackKnob : RoundKnob {};
struct RoundLargeBlackKnob : RoundKnob {};
struct RoundBigBlackKnob : RoundKnob {};
struct RoundHugeBlackKnob : RoundKnob {};
struct RoundBlackSnapKnob : RoundBlackKnob { RoundBlackSnapKnob() { snap = true; } };

struct Davies1900hKnob : SvgKnob {};
struct Davies1900hWhiteKnob : Davies1900hKnob {};
struct Davies1900hBlackKnob : Davies1900hKnob {};
struct Davies1900hRedKnob : Davies1900hKnob {};
struct Davies1900hLargeWhiteKnob : Davies1900hKnob {};
struct Davies1900hLargeBlackKnob : Davies1900hKnob {};
struct Davies1900hLargeRedKnob : Davies1900hKnob {};

struct Rogan : SvgKnob {};
struct Rogan6PSWhite : Rogan {};
struct Rogan5PSGray : Rogan {};
struct Rogan3PSBlue : Rogan {};
struct Rogan3PSRed : Rogan {};
struct Rogan3PSGreen : Rogan {};
struct Rogan3PSWhite : Rogan {};
struct Rogan3PBlue : Rogan {};
struct Rogan3PRed : Rogan {};
struct Rogan3PGreen : Rogan {};
struct Rogan3PWhite : Rogan {};
struct Rogan2SGray : Rogan {};
struct Rogan2PSBlue : Rogan {};
struct Rogan2PSRed : Rogan {};
struct Rogan2PSGreen : Rogan {};
struct Rogan2PSWhite : Rogan {};
struct Rogan2PBlue : Rogan {};
struct Rogan2PRed : Rogan {};
struct Rogan2PGreen : Rogan {};
struct Rogan2PWhite : Rogan {};
struct Rogan1PSBlue : Rogan {};
struct Rogan1PSRed : Rogan {};
struct Rogan1PSGreen : Rogan {};
struct Rogan1PSWhite : Rogan {};
struct Rogan1PBlue : Rogan {};
struct Rogan1PRed : Rogan {};
struct Rogan1PGreen : Rogan {};
struct Rogan1PWhite : Rogan {};

struct SynthTechAlco : SvgKnob {};
struct Trimpot : SvgKnob {};
struct BefacoBigKnob : SvgKnob {};
struct BefacoTinyKnob : SvgKnob {};
struct BefacoSlidePot : SvgSlider {};
struct VCVSlider : SvgSlider {};
typedef VCVSlider LEDSlider;
struct VCVSliderHorizontal : SvgSlider {};
typedef VCVSliderHorizontal LEDSliderHorizontal;

template <typename TBase, typename TLightBase = RedLight>
struct LightSlider : TBase {
    ModuleLightWidget* light = nullptr;
    ModuleLightWidget* getLight() { return light; }
};
template <typename TBase>
struct VCVSliderLight : RectangleLight<TSvgLight<TBase>> {};
template <typename TLightBase>
using LEDSliderLight = VCVSliderLight<TLightBase>;
template <typename TLightBase = RedLight>
struct VCVLightSlider : LightSlider<VCVSlider, TLightBase> {};
template <typename TLightBase = RedLight>
using LEDLightSlider = VCVLightSlider<TLightBase>;
struct LEDSliderGreen : VCVSlider {};
struct LEDSliderRed : VCVSlider {};
struct LEDSliderYellow : VCVSlider {};
struct LEDSliderBlue : VCVSlider {};
struct LEDSliderWhite : VCVSlider {};
template <typename TLightBase = RedLight>
struct VCVLightSliderHorizontal : LightSlider<VCVSliderHorizontal, TLightBase> {};
template <typename TLightBase>
using LEDLightSliderHorizontal = VCVLightSliderHorizontal<TLightBase>;

struct PJ301MPort : SvgPort {};
struct PJ3410Port : SvgPort {};
struct ThemedPJ301MPort : SvgPort {};
struct DarkPJ301MPort : SvgPort {};
struct CL1362Port : SvgPort {};

struct NKK : SvgSwitch {};
struct CKSS : SvgSwitch {};
struct CKSSThree : SvgSwitch {};
struct CKSSHorizontal : SvgSwitch {};
struct CKSSThreeHorizontal : SvgSwitch {};
struct CKD6 : SvgSwitch {};
struct TL1105 : SvgSwitch {};
struct VCVButton : SvgSwitch { VCVButton() { momentary = true; } };
typedef VCVButton LEDButton;
struct VCVLatch : VCVButton { VCVLatch() { momentary = false; latch = true; } };
template <typename TLight>
struct VCVLightButton : VCVButton {
    TLight* light = nullptr;
    VCVLightButton() { light = new TLight; this->addChild(light); }
    TLight* getLight() { return light; }
};
template <typename TLight>
using LEDLightButton = VCVLightButton<TLight>;
template <typename TLight>
struct VCVLightLatch : VCVLightButton<TLight> { VCVLightLatch() { this->momentary = false; this->latch = true; } };
struct VCVBezel : SvgSwitch { VCVBezel() { momentary = true; } };
typedef VCVBezel LEDBezel;
template <typename TLightBase = WhiteLight>
struct VCVLightBezel : VCVBezel {
    ModuleLightWidget* light = nullptr;
    ModuleLightWidget* getLight() { return light; }
};
template <typename TLightBase>
using LEDLightBezel = VCVLightBezel<TLightBase>;
struct PB61303 : SvgSwitch { PB61303() { momentary = true; } };
struct BefacoSwitch : SvgSwitch {};
struct BefacoPush : SvgSwitch { BefacoPush() { momentary = true; } };

struct ScrewSilver : SvgScrew {};
struct ScrewBlack : SvgScrew {};
struct ThemedScrew : SvgScrew {};

struct SegmentDisplay : widget::Widget {};

} // namespace componentlibrary

using namespace componentlibrary;

// ---- misc helpers referenced by widget code ---------------------------------

template <class TWidget>
TWidget* createWidget(math::Vec pos) {
    TWidget* widget = new TWidget;
    widget->box.pos = pos;
    return widget;
}
template <class TWidget>
TWidget* createWidgetCentered(math::Vec pos) {
    return createWidget<TWidget>(pos);
}
template <class TParamWidget>
TParamWidget* createParam(math::Vec pos, engine::Module* module, int paramId) {
    TParamWidget* widget = new TParamWidget;
    widget->box.pos = pos;
    widget->module = module;
    widget->paramId = paramId;
    return widget;
}
template <class TParamWidget>
TParamWidget* createParamCentered(math::Vec pos, engine::Module* module, int paramId) {
    return createParam<TParamWidget>(pos, module, paramId);
}
template <class TPortWidget>
TPortWidget* createInput(math::Vec pos, engine::Module* module, int portId) {
    TPortWidget* widget = new TPortWidget;
    widget->box.pos = pos;
    widget->module = module;
    widget->portId = portId;
    widget->type = engine::Port::INPUT;
    return widget;
}
template <class TPortWidget>
TPortWidget* createInputCentered(math::Vec pos, engine::Module* module, int portId) {
    return createInput<TPortWidget>(pos, module, portId);
}
template <class TPortWidget>
TPortWidget* createOutput(math::Vec pos, engine::Module* module, int portId) {
    TPortWidget* widget = new TPortWidget;
    widget->box.pos = pos;
    widget->module = module;
    widget->portId = portId;
    widget->type = engine::Port::OUTPUT;
    return widget;
}
template <class TPortWidget>
TPortWidget* createOutputCentered(math::Vec pos, engine::Module* module, int portId) {
    return createOutput<TPortWidget>(pos, module, portId);
}
template <class TModuleLightWidget>
TModuleLightWidget* createLight(math::Vec pos, engine::Module* module, int firstLightId) {
    TModuleLightWidget* widget = new TModuleLightWidget;
    widget->box.pos = pos;
    widget->module = module;
    widget->firstLightId = firstLightId;
    return widget;
}
template <class TModuleLightWidget>
TModuleLightWidget* createLightCentered(math::Vec pos, engine::Module* module, int firstLightId) {
    return createLight<TModuleLightWidget>(pos, module, firstLightId);
}
template <class TParamWidget>
TParamWidget* createLightParam(math::Vec pos, engine::Module* module, int paramId, int) {
    return createParam<TParamWidget>(pos, module, paramId);
}
template <class TParamWidget>
TParamWidget* createLightParamCentered(math::Vec pos, engine::Module* module, int paramId, int firstLightId) {
    return createLightParam<TParamWidget>(pos, module, paramId, firstLightId);
}
template <class TMenu = ui::Menu>
TMenu* createMenu() { return new TMenu; }
inline ui::MenuLabel* createMenuLabel(std::string text) {
    ui::MenuLabel* label = new ui::MenuLabel;
    label->text = std::move(text);
    return label;
}
template <class TMenuItem = ui::MenuItem>
TMenuItem* createMenuItem(std::string text, std::string rightText = "") {
    TMenuItem* item = new TMenuItem;
    item->text = std::move(text);
    item->rightText = std::move(rightText);
    return item;
}
inline app::SvgPanel* createPanel(std::string) { return new app::SvgPanel; }

} // namespace rack

#endif
