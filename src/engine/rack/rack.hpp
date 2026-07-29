//----------------------------------------------------------------------------
//  src/engine/rack/rack.hpp
//
//  A self-contained, GUI-free re-implementation of the VCV Rack v2 MODULE API
//  (rack::engine::Module / Param / Port / Light + config*()).  Module DSP code
//  written against VCV Rack ports here with minimal edits, because the API shape
//  -- params/inputs/outputs/lights arrays + process(ProcessArgs) -- is identical.
//  What we deliberately DROP is everything GUI/OpenGL/plugin-system: there is no
//  NanoVG, no ModuleWidget, no SVG.  Instead the modules are DATA-DRIVEN (each
//  declares its params/ports/lights + names), so one generic SDL renderer draws
//  every module without per-module GUI code.  That is what makes porting bounded.
//
//  Conventions (match VCV so modules interoperate):
//    * 1V/octave pitch CV, 0V == C4 (rack::FREQ_C4).
//    * audio  = +/-5V,  CV = +/-10V,  gate/trigger = 0/10V.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_RACK_RACK_HPP
#define PATCHKNOB_ENGINE_RACK_RACK_HPP

#include <vector>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cmath>

#ifndef ENUMS
#define ENUMS(name, count) name, name ## _LAST = name + (count) - 1
#endif

struct json_t {};

inline json_t* json_object() { return new json_t; }
inline json_t* json_array() { return new json_t; }
inline json_t* json_boolean(int) { return new json_t; }
inline json_t* json_true() { return new json_t; }
inline json_t* json_false() { return new json_t; }
inline json_t* json_null() { return new json_t; }
inline json_t* json_integer(long long) { return new json_t; }
inline json_t* json_real(double) { return new json_t; }
inline json_t* json_string(const char*) { return new json_t; }
inline json_t* json_stringn(const char*, size_t) { return new json_t; }
inline void json_object_set_new(json_t*, const char*, json_t* value) { delete value; }
inline void json_object_set(json_t*, const char*, json_t*) {}
inline void json_array_insert_new(json_t*, size_t, json_t* value) { delete value; }
inline void json_array_append_new(json_t*, json_t* value) { delete value; }
inline void json_array_append(json_t*, json_t*) {}
inline void json_object_update(json_t*, json_t*) {}
inline json_t* json_object_get(json_t*, const char*) { return nullptr; }
inline json_t* json_array_get(json_t*, size_t) { return nullptr; }
inline size_t json_array_size(json_t*) { return 0; }
inline size_t json_object_size(json_t*) { return 0; }
inline int json_boolean_value(json_t*) { return 0; }
inline long long json_integer_value(json_t*) { return 0; }
inline double json_real_value(json_t*) { return 0.0; }
inline double json_number_value(json_t*) { return 0.0; }
inline const char* json_string_value(json_t*) { return ""; }
inline size_t json_string_length(json_t*) { return 0; }
inline int json_is_true(json_t*) { return 0; }
inline int json_is_false(json_t*) { return 0; }
inline int json_is_boolean(json_t*) { return 0; }
inline int json_is_integer(json_t*) { return 0; }
inline int json_is_real(json_t*) { return 0; }
inline int json_is_number(json_t*) { return 0; }
inline int json_is_string(json_t*) { return 0; }
inline int json_is_array(json_t*) { return 0; }
inline int json_is_object(json_t*) { return 0; }
inline int json_is_null(json_t*) { return 0; }
inline json_t* json_incref(json_t* value) { return value; }
inline void json_decref(json_t* value) { delete value; }
inline json_t* json_copy(json_t*) { return nullptr; }
inline json_t* json_deep_copy(json_t*) { return nullptr; }
inline json_t* json_loads(const char*, size_t, void*) { return nullptr; }
inline json_t* json_load_file(const char*, size_t, void*) { return nullptr; }
inline char* json_dumps(json_t*, size_t) { return nullptr; }
inline int json_dump_file(json_t*, const char*, size_t) { return -1; }

#ifndef json_array_foreach
#define json_array_foreach(array, index, value) \
    for ((index) = 0; (index) < json_array_size(array) && ((value) = json_array_get(array, index)); ++(index))
#endif
#ifndef json_object_foreach
#define json_object_foreach(object, key, value) \
    for ((key) = nullptr; false;)
#endif

namespace rack {

// ---- math (subset used by module DSP) --------------------------------------
namespace math {
inline float clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline int   clamp(int x, int lo, int hi)       { return x < lo ? lo : (x > hi ? hi : x); }
inline float rescale(float x, float xMin, float xMax, float yMin, float yMax) {
    return yMin + (x - xMin) / (xMax - xMin) * (yMax - yMin);
}
template <typename T>
inline T crossfade(T a, T b, T p) { return a + (b - a) * p; }
inline float normalizeZero(float x) { return std::isfinite(x) ? x : 0.f; }
struct Vec {
    float x = 0.f, y = 0.f;
    Vec() {}
    Vec(float X, float Y) : x(X), y(Y) {}
    Vec plus(const Vec& other) const { return Vec(x + other.x, y + other.y); }
    Vec minus(const Vec& other) const { return Vec(x - other.x, y - other.y); }
    Vec mult(float scale) const { return Vec(x * scale, y * scale); }
    Vec mult(const Vec& other) const { return Vec(x * other.x, y * other.y); }
    Vec div(float scale) const { return Vec(x / scale, y / scale); }
    Vec div(const Vec& other) const { return Vec(x / other.x, y / other.y); }
    Vec neg() const { return Vec(-x, -y); }
    float norm() const { return std::hypot(x, y); }
    float square() const { return x * x + y * y; }
    Vec floor() const { return Vec(std::floor(x), std::floor(y)); }
    Vec ceil() const { return Vec(std::ceil(x), std::ceil(y)); }
    Vec round() const { return Vec(std::round(x), std::round(y)); }
    bool isZero() const { return x == 0.f && y == 0.f; }
    bool isEqual(const Vec& other) const { return x == other.x && y == other.y; }
};
inline Vec operator+(const Vec& a, const Vec& b) { return Vec(a.x + b.x, a.y + b.y); }
inline Vec operator-(const Vec& a, const Vec& b) { return Vec(a.x - b.x, a.y - b.y); }
inline Vec operator-(const Vec& a) { return Vec(-a.x, -a.y); }
inline Vec operator*(const Vec& a, float s) { return Vec(a.x * s, a.y * s); }
inline Vec operator*(float s, const Vec& a) { return Vec(a.x * s, a.y * s); }
inline Vec operator/(const Vec& a, float s) { return Vec(a.x / s, a.y / s); }
struct Rect {
    Vec pos;
    Vec size;
    Rect() {}
    Rect(Vec position, Vec dimensions) : pos(position), size(dimensions) {}
    Rect(float x, float y, float w, float h) : pos(x, y), size(w, h) {}
    Vec getCenter() const { return Vec(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f); }
    Vec getTopLeft() const { return pos; }
    Vec getBottomRight() const { return Vec(pos.x + size.x, pos.y + size.y); }
    float getWidth() const { return size.x; }
    float getHeight() const { return size.y; }
    bool contains(const Vec& point) const {
        return point.x >= pos.x && point.x < pos.x + size.x &&
               point.y >= pos.y && point.y < pos.y + size.y;
    }
};
} // namespace math
using math::clamp;
using math::rescale;
using math::crossfade;
using math::Vec;
using math::Rect;

// Rack renders SVGs at 75 DPI: 1px == 25.4/75 mm.
constexpr float MM_PER_PX = 25.4f / 75.f;
inline float mm2px(float mm) { return mm / MM_PER_PX; }
inline math::Vec mm2px(math::Vec mm) { return math::Vec(mm2px(mm.x), mm2px(mm.y)); }
inline float px2mm(float px) { return px * MM_PER_PX; }
inline math::Vec px2mm(math::Vec px) { return math::Vec(px2mm(px.x), px2mm(px.y)); }
namespace window {
using rack::mm2px;
using rack::px2mm;
}

inline int eucMod(int value, int divisor) {
    if (divisor == 0) return 0;
    const int remainder = value % divisor;
    return remainder < 0 ? remainder + std::abs(divisor) : remainder;
}

constexpr float FREQ_C4 = 261.6256f;    // Hz at 0V pitch CV (1V/oct)
constexpr float FREQ_A4 = 440.f;

// ---- plugin/model surface (declaration-level: bridge DSP only needs the
//      types to exist so plugin.hpp headers parse; models are never registered
//      through this path -- the bridge manifest carries the metadata) ---------
namespace plugin {
struct Model;
struct Plugin {
    std::string slug;
    void addModel(Model*) {}
};
struct Model {
    Plugin*     plugin = nullptr;
    std::string slug;
    std::string name;
};
} // namespace plugin
using plugin::Model;
using plugin::Plugin;

template <typename TModule, typename TWidget = void>
plugin::Model* createModel(const std::string& = "") { return nullptr; }

// ---- app context: APP->engine->getSampleRate() and friends ------------------
// One instance per bridge DLL (inline globals).  The base
// Module::onSampleRateChange(float) hook keeps it current, because the PatchKnob
// engine broadcasts that call on every rate change and at module creation.
namespace window { struct Window; }
namespace history { struct State; }
namespace engine { struct Module; }

namespace app_compat {
struct Engine {
    float   sampleRate = 48000.f;
    float   sampleTime = 1.f / 48000.f;
    int64_t frame      = 0;
    float   getSampleRate() const { return sampleRate; }
    float   getSampleTime() const { return sampleTime; }
    int64_t getFrame() const { return frame; }
    int64_t getBlockFrame() const { return frame; }
    int64_t getStepFrame() const { return frame; }
    engine::Module* getModule(int64_t) { return nullptr; }
    std::vector<int64_t> getModuleIds() { return {}; }
    void randomizeModule(engine::Module*) {}
    void resetModule(engine::Module*) {}
};
struct Context {
    Engine  engineStorage;
    Engine* engine = &engineStorage;
    // set by rack_widgets_compat.hpp in bridge builds; null in the host
    rack::window::Window* window = nullptr;
    rack::history::State* history = nullptr;
};
inline Context& context() { static Context value; return value; }
inline void setSampleRate(float sampleRate) {
    if (!(sampleRate > 0.f)) return;
    context().engineStorage.sampleRate = sampleRate;
    context().engineStorage.sampleTime = 1.f / sampleRate;
}
} // namespace app_compat
inline app_compat::Context* contextGet() { return &app_compat::context(); }

namespace engine {

static const int PORT_MAX_CHANNELS = 16;    // polyphony cap (VCV = 16)

struct Module;

// ---- Param: one knob/slider/switch value -----------------------------------
struct Param {
    float value = 0.f;
    float getValue() const { return value; }
    void  setValue(float v) { value = v; }
};

// ---- Light: one LED brightness ---------------------------------------------
struct Light {
    float value = 0.f;
    float red = 0.f, green = 0.f, blue = 0.f;
    bool  color = false;
    void  setBrightness(float b) { value = b; color = false; }
    float getBrightness() const { return value; }
    void  setBrightnessRGB(float r, float g, float b) {
        red = r; green = g; blue = b; color = true;
        value = std::fmax(r, std::fmax(g, b));
    }
    bool  isColor() const { return color; }
    float getRed() const { return red; }
    float getGreen() const { return green; }
    float getBlue() const { return blue; }
    // Rise instantly, fall exponentially (VCV setBrightnessSmooth semantics).
    void  setBrightnessSmooth(float b, float dt, float lambda = 30.f) {
        color = false;
        if (b < value) value += (b - value) * lambda * dt;
        else           value = b;
    }
    void  setSmoothBrightness(float b, float dt) { setBrightnessSmooth(b, dt); }
};

// ---- Port: an audio/CV inlet or outlet, up to 16 poly channels -------------
struct Port {
    enum Type { INPUT, OUTPUT };
    float   voltages[PORT_MAX_CHANNELS] = {};
    float   value = 0.f;                // legacy Rack scalar alias for channel 0
    uint8_t channels = 0;               // 0 == disconnected
    bool    connected = false;          // cable presence, independent of signal channels

    float getVoltage(int c = 0) const { return voltages[c]; }
    float getPolyVoltage(int c) const { return channels == 1 ? voltages[0] : voltages[c]; }
    float getNormalVoltage(float normal, int c = 0) const { return channels > 0 ? voltages[c] : normal; }
    float getNormalPolyVoltage(float normal, int c) const { return channels > 0 ? getPolyVoltage(c) : normal; }
    void  setVoltage(float v, int c = 0) { voltages[c] = v; if (c == 0) value = v; }
    float* getVoltages() { return voltages; }
    const float* getVoltages() const { return voltages; }
    float* getVoltages(int channel) { return voltages + rack::math::clamp(channel, 0, PORT_MAX_CHANNELS - 1); }
    const float* getVoltages(int channel) const { return voltages + rack::math::clamp(channel, 0, PORT_MAX_CHANNELS - 1); }
    void readVoltages(float* values) const {
        for (int channel = 0; channel < PORT_MAX_CHANNELS; ++channel)
            values[channel] = voltages[channel];
    }
    void writeVoltages(const float* values) {
        for (int channel = 0; channel < PORT_MAX_CHANNELS; ++channel)
            voltages[channel] = values[channel];
        value = voltages[0];
    }
    template <typename T>
    T getVoltageSimd(int channel) const {
        float values[4] = {};
        for (int index = 0; index < 4 && channel + index < PORT_MAX_CHANNELS; ++index)
            values[index] = getVoltage(channel + index);
        return T::load(values);
    }
    template <typename T>
    T getPolyVoltageSimd(int channel) const {
        float values[4] = {};
        for (int index = 0; index < 4 && channel + index < PORT_MAX_CHANNELS; ++index)
            values[index] = getPolyVoltage(channel + index);
        return T::load(values);
    }
    template <typename T>
    void setVoltageSimd(T value, int channel) {
        float values[4] = {};
        value.store(values);
        for (int index = 0; index < 4 && channel + index < PORT_MAX_CHANNELS; ++index)
            setVoltage(values[index], channel + index);
    }
    // channels is a public uint8_t (0..255) a module can set without going
    // through setChannels; every read that bounds a voltages[] loop clamps to
    // PORT_MAX_CHANNELS so a corrupt count can never index out of bounds.
    float getVoltageSum() const {
        const int n = channels > PORT_MAX_CHANNELS ? PORT_MAX_CHANNELS : channels;
        float s = 0.f; for (int i = 0; i < n; ++i) s += voltages[i]; return s;
    }
    int   getChannels() const { return channels > PORT_MAX_CHANNELS ? PORT_MAX_CHANNELS : channels; }
    bool  isPolyphonic() const { return getChannels() > 1; }
    void  setChannels(int c) {
        c = rack::math::clamp(c, 0, PORT_MAX_CHANNELS);
        for (int i = c; i < channels; ++i) voltages[i] = 0.f;   // clear dropped channels
        channels = (uint8_t)c;
    }
    bool  isConnected() const { return connected || channels > 0; }
    void  clearVoltages() {
        for (int i = 0; i < PORT_MAX_CHANNELS; ++i) voltages[i] = 0.f;
        value = 0.f;
    }
};
struct Input  : Port {};
struct Output : Port {};

struct PortInfo {
    std::string name;
    std::string description;
};

// ---- ParamQuantity: a param's metadata (range/name/unit/display) -----------
// configParam() returns a pointer so module ctors can tweak fields VCV-style.
struct ParamQuantity {
    Module*     module           = nullptr;
    int         paramId          = -1;
    float       minValue         = 0.f;
    float       maxValue         = 1.f;
    float       defaultValue     = 0.f;
    std::string name;
    std::string unit;
    float       displayBase       = 0.f;   // 0 linear; >0 exp base; <0 log
    float       displayMultiplier = 1.f;
    float       displayOffset     = 0.f;
    bool        snapEnabled       = false;  // integer/switch
    bool        randomizeEnabled  = true;
    // A few fields VCV module ctors sometimes set; kept as harmless storage.
    float       smoothEnabled     = false;
    std::string description;
    std::vector<std::string> labels;        // for switches
    virtual ~ParamQuantity() = default;
    virtual float getDisplayValue();
    // Appended API (keep append-only: paramQuantities is indexed by value
    // across the bridge ABI, so fields must not change; new virtuals go after
    // getDisplayValue).
    float getValue() const;
    void setValue(float value);
    float getMinValue() const { return minValue; }
    float getMaxValue() const { return maxValue; }
    float getDefaultValue() const { return defaultValue; }
    float getRange() const { return maxValue - minValue; }
    bool isBounded() const { return true; }
    bool isMin() const { return getValue() <= minValue; }
    bool isMax() const { return getValue() >= maxValue; }
    void setMin() { setValue(minValue); }
    void setMax() { setValue(maxValue); }
    void reset() { setValue(defaultValue); }
    void toggle() { setValue(getValue() > minValue ? minValue : maxValue); }
    float getScaledValue() const {
        return maxValue == minValue ? 0.f : (getValue() - minValue) / (maxValue - minValue);
    }
    void setScaledValue(float scaled) { setValue(minValue + scaled * (maxValue - minValue)); }
    void setSmoothValue(float value) { setValue(value); }
    float getSmoothValue() const { return getValue(); }
    void setImmediateValue(float value) { setValue(value); }
    float getImmediateValue() const { return getValue(); }
    virtual void setDisplayValue(float displayValue) {
        if (displayBase > 0.f && displayMultiplier != 0.f)
            setValue(std::log((displayValue - displayOffset) / displayMultiplier) / std::log(displayBase));
        else if (displayMultiplier != 0.f)
            setValue((displayValue - displayOffset) / displayMultiplier);
    }
    virtual std::string getDisplayValueString();
    virtual void setDisplayValueString(std::string) {}
    virtual std::string getLabel() { return name; }
    virtual std::string getUnit() { return unit; }
    virtual std::string getString();
    virtual std::string getDescription() { return description; }
};

struct SampleRateChangeEvent {
    float sampleRate = 48000.f;
    float sampleTime = 1.f / 48000.f;
};

// ---- Module: the unit of DSP + declared I/O --------------------------------
struct Module {
    std::vector<Param>         params;
    std::vector<Input>         inputs;
    std::vector<Output>        outputs;
    std::vector<Light>         lights;
    std::vector<ParamQuantity> paramQuantities;   // parallel to params
    std::vector<std::string>   inputInfos;         // parallel to inputs  (labels)
    std::vector<std::string>   outputInfos;        // parallel to outputs
    std::vector<std::string>   lightInfos;         // parallel to lights
    struct BypassRoute { int inputId = -1; int outputId = -1; };
    std::vector<BypassRoute>   bypassRoutes;

    struct ProcessArgs {
        float   sampleRate = 48000.f;
        float   sampleTime = 1.f / 48000.f;
        float   tempoBpm   = 120.f;
        bool    isPlaying  = true;
        int64_t frame      = 0;
    };
    virtual ~Module() {}

    void config(int numParams, int numInputs, int numOutputs, int numLights = 0) {
        params.resize(numParams);
        inputs.resize(numInputs);
        outputs.resize(numOutputs);
        lights.resize(numLights);
        paramQuantities.resize(numParams);
        inputInfos.resize(numInputs);
        outputInfos.resize(numOutputs);
        lightInfos.resize(numLights);
        bypassRoutes.clear();
    }

    ParamQuantity* configParam(int paramId, float minValue, float maxValue, float defaultValue,
                               std::string name = "", std::string unit = "",
                               float displayBase = 0.f, float displayMultiplier = 1.f,
                               float displayOffset = 0.f) {
        if (paramId < 0 || paramId >= (int)params.size()) return nullptr;
        params[paramId].value = defaultValue;
        ParamQuantity& q = paramQuantities[paramId];
        q.module = this; q.paramId = paramId; q.minValue = minValue; q.maxValue = maxValue;
        q.defaultValue = defaultValue; q.name = name; q.unit = unit;
        q.displayBase = displayBase; q.displayMultiplier = displayMultiplier;
        q.displayOffset = displayOffset;
        return &q;
    }
    template <typename Quantity>
    Quantity* configParam(int paramId, float minValue, float maxValue, float defaultValue,
                          std::string name = "", std::string unit = "",
                          float displayBase = 0.f, float displayMultiplier = 1.f,
                          float displayOffset = 0.f) {
        return reinterpret_cast<Quantity*>(configParam(paramId, minValue, maxValue, defaultValue,
                                                        std::move(name), std::move(unit), displayBase,
                                                        displayMultiplier, displayOffset));
    }
    // A switch is a snapped param (integer positions).
    ParamQuantity* configSwitch(int paramId, float minValue, float maxValue, float defaultValue,
                                std::string name = "", std::vector<std::string> labels = {}) {
        ParamQuantity* q = configParam(paramId, minValue, maxValue, defaultValue, name);
        if (q) { q->snapEnabled = true; q->labels = labels; }
        return q;
    }
    ParamQuantity* configButton(int paramId, std::string name = "") {
        return configParam(paramId, 0.f, 1.f, 0.f, name);
    }
    ParamQuantity* getParamQuantity(int id) {
        return id >= 0 && id < (int)paramQuantities.size() ? &paramQuantities[id] : nullptr;
    }
    const ParamQuantity* getParamQuantity(int id) const {
        return id >= 0 && id < (int)paramQuantities.size() ? &paramQuantities[id] : nullptr;
    }
    // Returns a scratch PortInfo so `configInput(...)->description = ...` code
    // compiles; only the name lands in inputInfos/outputInfos.
    PortInfo* configInput (int id, std::string name = "") {
        if (id >= 0 && id < (int)inputInfos.size()) inputInfos[id] = name;
        static thread_local PortInfo scratch;
        scratch = PortInfo{std::move(name), {}};
        return &scratch;
    }
    PortInfo* configOutput(int id, std::string name = "") {
        if (id >= 0 && id < (int)outputInfos.size()) outputInfos[id] = name;
        static thread_local PortInfo scratch;
        scratch = PortInfo{std::move(name), {}};
        return &scratch;
    }
    void configLight (int id, std::string name = "") { if (id >= 0 && id < (int)lightInfos.size())   lightInfos[id]  = name; }
    void configBypass(int inputId, int outputId) { bypassRoutes.push_back({inputId, outputId}); }

    // VTABLE ABI: the engine and pre-built bridge DLLs dispatch through these
    // slots.  NEVER reorder or insert above an existing virtual -- new virtuals
    // and new data members go in the appended block at the end of the struct.
    virtual void process(const ProcessArgs& args) { (void)args; step(); }
    virtual void onReset() {}
    virtual void onRandomize() {}
    virtual void onSampleRateChange(float sampleRate) {
        rack::app_compat::setSampleRate(sampleRate);
        onSampleRateChange(SampleRateChangeEvent{sampleRate, 1.f / sampleRate});
    }
    virtual void onSampleRateChange(const SampleRateChangeEvent&) { onSampleRateChange(); }
    virtual json_t* dataToJson() { return nullptr; }
    virtual void dataFromJson(json_t*) {}
    virtual void paramsFromJson(json_t*) {}
    virtual json_t* toJson() { return nullptr; }
    virtual void fromJson(json_t*) {}

    // ---- appended API surface (keep append-only; see ABI note above) -------
    struct ResetEvent {};
    struct RandomizeEvent {};
    struct AddEvent {};
    struct RemoveEvent {};
    struct BypassEvent {};
    struct UnBypassEvent {};
    struct PortChangeEvent {
        bool connecting = false;
        Port::Type type = Port::INPUT;
        int portId = -1;
    };
    struct ExpanderChangeEvent {
        bool side = false;  // false = left, true = right
    };
    struct Expander {
        Module* module = nullptr;
        void*   producerMessage = nullptr;
        void*   consumerMessage = nullptr;
        bool    messageFlipRequested = false;
        void requestMessageFlip() { messageFlipRequested = true; }
    };

    // Legacy VCV v1 hook; the SampleRateChangeEvent overload chains here.
    virtual void onSampleRateChange() {}
    // Event-form hooks (VCV v2).  The engine still calls the legacy slots
    // directly, so default event bodies chain down to them.
    virtual void onReset(const ResetEvent&) { onReset(); }
    virtual void onRandomize(const RandomizeEvent&) { onRandomize(); }
    virtual void onAdd(const AddEvent&) { onAdd(); }
    virtual void onRemove(const RemoveEvent&) { onRemove(); }
    virtual void onBypass(const BypassEvent&) {}
    virtual void onUnBypass(const UnBypassEvent&) {}
    virtual void onPortChange(const PortChangeEvent&) {}
    virtual void onExpanderChange(const ExpanderChangeEvent&) {}
    // VCV v1 legacy per-sample hook; the default process() chains here so
    // v1-style modules (`void step() override`) run unmodified.
    virtual void step() {}
    // VCV v1 legacy lifecycle hooks; event versions chain here.
    virtual void onAdd() {}
    virtual void onRemove() {}

    Param&  getParam(int index) { return params[index]; }
    Input&  getInput(int index) { return inputs[index]; }
    Output& getOutput(int index) { return outputs[index]; }
    Light&  getLight(int index) { return lights[index]; }
    int     getNumParams() const { return (int)params.size(); }
    int     getNumInputs() const { return (int)inputs.size(); }
    int     getNumOutputs() const { return (int)outputs.size(); }
    int     getNumLights() const { return (int)lights.size(); }
    int64_t getId() const { return id; }
    plugin::Model* getModel() const { return model; }
    Expander& getLeftExpander() { return leftExpander; }
    Expander& getRightExpander() { return rightExpander; }

    // Appended data members (host code must not touch these on modules from
    // bridge DLLs built before they existed).
    int64_t        id = -1;
    plugin::Model* model = nullptr;
    Expander       leftExpander;
    Expander       rightExpander;
};

inline float ParamQuantity::getDisplayValue() {
    if (!module || paramId < 0 || paramId >= (int)module->params.size()) return defaultValue;
    float value = module->params[paramId].getValue();
    if (displayBase > 0.f) value = std::pow(displayBase, value);
    return value * displayMultiplier + displayOffset;
}

inline float ParamQuantity::getValue() const {
    if (!module || paramId < 0 || paramId >= (int)module->params.size()) return defaultValue;
    return module->params[paramId].getValue();
}

inline void ParamQuantity::setValue(float value) {
    if (!module || paramId < 0 || paramId >= (int)module->params.size()) return;
    module->params[paramId].setValue(value < minValue ? minValue : (value > maxValue ? maxValue : value));
}

inline std::string ParamQuantity::getDisplayValueString() {
    char text[64];
    std::snprintf(text, sizeof(text), "%g", getDisplayValue());
    return text;
}

inline std::string ParamQuantity::getString() {
    std::string result = name.empty() ? "" : name + ": ";
    return result + getDisplayValueString() + unit;
}

} // namespace engine

using engine::Input;
using engine::Light;
using engine::Module;
using engine::Output;
using engine::Param;
using engine::ParamQuantity;
using engine::SampleRateChangeEvent;
} // namespace rack

#endif
