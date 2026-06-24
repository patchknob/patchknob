//----------------------------------------------------------------------------
//  seq24 Windows port — shared engine contract.
//
//  This header is the STABLE INTERFACE that the parallel module agents code
//  against so their work integrates cleanly:
//    * the VST2 host and VST3 host each implement IPluginInstance
//    * the plugin scanner produces PluginDescriptor lists
//    * the audio engine / track graph consume IPluginInstance only (never the
//      concrete VST2/VST3 classes)
//
//  Audio buffer convention (the whole engine uses this everywhere):
//    * sample type        : float
//    * layout             : NON-INTERLEAVED (planar): float** = array of
//                           per-channel pointers, each pointing at `nframes`
//                           contiguous floats.
//    * sample rate / block: fixed for the lifetime of a prepared instance,
//                           (re)set via IPluginInstance::prepare().
//    * realtime rule      : process() and everything it calls must be
//                           lock-free and allocation-free (audio thread).
//----------------------------------------------------------------------------
#ifndef SEQ24_ENGINE_PLUGIN_API_H
#define SEQ24_ENGINE_PLUGIN_API_H

#include <string>
#include <vector>
#include <cstdint>

namespace seq24 { namespace engine {

enum class PluginFormat { VST2, VST3 };

// A plugin discovered on disk (by the scanner). `uid` disambiguates classes
// inside a multi-class VST3 bundle or a VST2 shell; pass it back to the host to
// instantiate the exact sub-plugin.
struct PluginDescriptor {
    PluginFormat format;
    std::string  name;        // display name
    std::string  vendor;
    std::string  path;        // .dll (VST2) or .vst3 (VST3) on disk
    std::string  uid;         // class/sub-plugin id within the file ("" = first)
    bool         isInstrument = false;  // emits audio in response to MIDI
    int          numAudioIn   = 0;
    int          numAudioOut  = 0;
};

// One automatable/controllable parameter.
struct ParamInfo {
    uint32_t    id;           // host-stable id (VST3 ParamID; VST2 index)
    std::string name;
    float       defaultValue; // normalized 0..1
};

// A simple sample-accurate MIDI event handed to a plugin for one process block.
// (Note on/off/CC etc. — raw 3-byte short message + offset within the block.)
struct MidiEvent {
    int32_t       sampleOffset;  // 0..nframes-1
    uint8_t       status;        // e.g. 0x90 | channel
    uint8_t       data1;
    uint8_t       data2;
};

// A parameter change to apply at a sample offset within the block (automation).
struct ParamChange {
    uint32_t id;
    int32_t  sampleOffset;
    float    value;              // normalized 0..1
};

// Buffers passed to process() for one block. Inputs may be null for synths.
struct ProcessBlock {
    const float* const* audioIn;   // [numAudioIn][nframes] or null
    float* const*       audioOut;  // [numAudioOut][nframes]
    int32_t             nframes;
    const MidiEvent*    midiIn;     // events for this block (sorted by offset)
    int32_t             numMidiIn;
    const ParamChange*  paramIn;    // automation for this block
    int32_t             numParamIn;
    double              tempoBpm;
    int64_t             playPositionSamples;
    bool                isPlaying;
};

// Opaque native window handle for embedding the plugin editor (Win32 HWND).
typedef void* NativeWindowHandle;

// The ONE interface the rest of the engine sees. Both the VST2 and VST3 hosts
// return objects implementing this. All methods are called from the message
// thread EXCEPT process(), which is audio-thread / realtime.
class IPluginInstance {
public:
    virtual ~IPluginInstance() {}

    virtual const PluginDescriptor& descriptor() const = 0;

    // --- lifecycle (message thread) ---
    virtual bool prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void setActive(bool active) = 0;          // resume/suspend
    virtual void release() = 0;

    // --- realtime (audio thread) ---
    virtual void process(const ProcessBlock& blk) = 0;

    // --- parameters (message thread for query; changes via ParamChange in process) ---
    virtual int       paramCount() const = 0;
    virtual ParamInfo paramInfo(int index) const = 0;
    virtual float     getParamNormalized(uint32_t id) const = 0;
    virtual void      setParamNormalized(uint32_t id, float v) = 0;  // non-RT set

    // --- editor (message thread) ---
    virtual bool hasEditor() const = 0;
    virtual bool openEditor(NativeWindowHandle parent) = 0;          // embed in parent
    virtual void closeEditor() = 0;
    virtual void getEditorSize(int& w, int& h) const = 0;
    virtual void idleEditor() = 0;                                   // pump (VST2)

    // --- state (message thread) ---
    virtual std::vector<uint8_t> saveState() const = 0;
    virtual void                 loadState(const std::vector<uint8_t>& data) = 0;
};

// Factory: implemented by the unified plugin host. Hides VST2 vs VST3.
class IPluginHost {
public:
    virtual ~IPluginHost() {}
    // Scan the standard VST2/VST3 directories (and any extra paths) on this OS.
    virtual std::vector<PluginDescriptor> scan(const std::vector<std::string>& extraPaths) = 0;
    // Instantiate by descriptor. Returns null on failure. Caller owns the result.
    virtual IPluginInstance* instantiate(const PluginDescriptor& desc) = 0;
};

}} // namespace seq24::engine

#endif
