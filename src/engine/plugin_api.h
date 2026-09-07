//----------------------------------------------------------------------------
//  PatchKnob — shared engine contract.
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
#ifndef PATCHKNOB_ENGINE_PLUGIN_API_H
#define PATCHKNOB_ENGINE_PLUGIN_API_H

#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>

namespace PatchKnob { namespace engine {

enum class PluginFormat { VST2, VST3 };

//----------------------------------------------------------------------------
//  BUS LAYOUT
//
//  numAudioIn/numAudioOut are the FLAT channel totals summed over every bus --
//  that is their historical meaning and every existing reader keeps it.  But a
//  flat total on its own is not enough to route a plugin correctly:
//
//    * a drum machine with per-pad outputs, a sampler with per-key buses and a
//      synth with a separate FX-return bus all report one big total, and a host
//      that treats it as one channel list can only ever reach the first pair;
//    * worse, a plugin with a MONO MAIN bus plus an aux bus reports >= 2 and so
//      escaped the mono->stereo upmix entirely: caller channel 1 got the aux
//      bus's left channel instead of a copy of the main.
//
//  So the flat list stays, and the bus structure sits next to it.  BUS 0 IS THE
//  MAIN BUS -- that is the stable identity the routing keys on (both VST3 and
//  VST2 order the main bus first; `isMain` additionally records what the plugin
//  itself said, for display).  Everything that used to key on the flat total --
//  the mono upmix above all -- keys on bus 0's channelCount instead.
//----------------------------------------------------------------------------
struct PluginBusInfo {
    std::string name;              // plugin's own bus name ("Main", "Kick", ...)
    int         channelCount = 0;
    bool        isMain       = false;  // VST3 kMain / VST2 first bus
    bool        isAux        = false;  // VST3 kAux: side-chain or FX return
};

// PatchKnob's audio ports are STEREO (patch_graph.h kMaxBusChan == 2), so every
// plugin bus is given a stereo SLOT in the caller's flat channel array: bus 0
// starts at caller channel 0, bus 1 at 2, and so on.  Two consequences, both
// deliberate:
//
//   * a plugin whose main bus is stereo maps EXACTLY as it did before this
//     existed (0,1 = main), so nothing that works today changes;
//   * a MONO bus still occupies two caller channels, its single channel copied
//     into the second -- which is precisely the mono-main fix, and it keeps
//     every later bus aligned with the caller's stereo port for that bus.
//
// A bus WIDER than stereo keeps its full (even-rounded) width so that a host
// handed a matching flat array -- the VST2 test harness supplies all N channels
// of an N-out plugin -- still reaches every channel.  With a single bus the
// whole mapping therefore degenerates to plain flat packing, i.e. the legacy
// behaviour, which is also what an empty layout means.
constexpr int kPluginBusSlotChannels = 2;

inline int pluginBusSlotWidth(int channelCount)
{
    if (channelCount <= kPluginBusSlotChannels) return kPluginBusSlotChannels;
    return channelCount + (channelCount & 1);      // round odd widths up to even
}

// Channel count of the MAIN bus (bus 0).  `flatFallback` (numAudioIn/Out) is
// returned when the layout is unknown -- a descriptor from a pre-layout cache,
// or a plugin that answered no bus queries at all.
inline int pluginMainBusChannels(const std::vector<PluginBusInfo>& buses,
                                 int flatFallback)
{
    return buses.empty() ? flatFallback : buses[0].channelCount;
}

// How many caller channels the whole layout occupies (see the slot rule above).
inline int pluginBusCallerChannels(const std::vector<PluginBusInfo>& buses,
                                   int flatFallback)
{
    if (buses.empty()) return flatFallback;
    int total = 0;
    for (const PluginBusInfo& b : buses) total += pluginBusSlotWidth(b.channelCount);
    return total;
}

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
    int          numAudioIn   = 0;      // FLAT total over audioInBuses
    int          numAudioOut  = 0;      // FLAT total over audioOutBuses
    // Ordered bus layout; bus 0 is the main bus.  Empty == unknown (treat as a
    // single main bus of numAudioIn/numAudioOut channels).  Appended last so
    // positional aggregate init of the older fields stays valid.
    std::vector<PluginBusInfo> audioInBuses;
    std::vector<PluginBusInfo> audioOutBuses;

    int mainInChannels()  const { return pluginMainBusChannels(audioInBuses,  numAudioIn);  }
    int mainOutChannels() const { return pluginMainBusChannels(audioOutBuses, numAudioOut); }
};

//----------------------------------------------------------------------------
//  BUS LAYOUT WIRE FORMAT
//
//  ONE encoding, used by all three places a layout has to survive a boundary:
//  the out-of-process probes' stdout (BUSIN=/BUSOUT= lines), the scanner that
//  parses them, and the plugin cache file.  Keeping it here means the writer
//  and the reader cannot drift apart.
//
//    buses    := bus (';' bus)*
//    bus      := name '/' channelCount '/' flags
//    flags    := 'm'?  'a'?          ('m' = main bus, 'a' = aux/side-chain)
//
//  '\', ';' and '/' inside a name are backslash-escaped, so a bus called
//  "FX / Send" round-trips intact.  An empty string decodes to an empty layout,
//  which every consumer reads as "unknown -- treat as one main bus".
//----------------------------------------------------------------------------
inline std::string pluginEncodeBusLayout(const std::vector<PluginBusInfo>& buses)
{
    std::string out;
    for (size_t i = 0; i < buses.size(); ++i)
    {
        if (i) out += ';';
        for (char c : buses[i].name)
        {
            if (c == '\\' || c == ';' || c == '/') out += '\\';
            out += c;
        }
        out += '/';
        out += std::to_string(buses[i].channelCount);
        out += '/';
        if (buses[i].isMain) out += 'm';
        if (buses[i].isAux)  out += 'a';
    }
    return out;
}

// Inverse of pluginEncodeBusLayout. Returns false (and leaves `buses` empty) on
// malformed input, so a corrupt field degrades to "unknown layout" rather than
// to a wrong one.
inline bool pluginDecodeBusLayout(const std::string& text,
                                  std::vector<PluginBusInfo>& buses)
{
    buses.clear();
    if (text.empty()) return true;

    PluginBusInfo cur;
    std::string   field;
    int           fieldIndex = 0;      // 0 name, 1 channels, 2 flags

    auto endField = [&]() -> bool {
        if (fieldIndex == 0)      cur.name = field;
        else if (fieldIndex == 1)
        {
            if (field.empty()) return false;
            for (char c : field) if (c < '0' || c > '9') return false;
            cur.channelCount = std::atoi(field.c_str());
        }
        else if (fieldIndex == 2)
        {
            for (char c : field)
            {
                if      (c == 'm') cur.isMain = true;
                else if (c == 'a') cur.isAux  = true;
                else return false;      // unknown flag: refuse the whole layout
            }
        }
        else return false;              // too many '/' in one bus
        field.clear();
        ++fieldIndex;
        return true;
    };
    auto endBus = [&]() -> bool {
        if (!endField()) return false;
        if (fieldIndex < 2) return false;          // needs at least name/count
        buses.push_back(cur);
        cur = PluginBusInfo{};
        fieldIndex = 0;
        return true;
    };

    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '\\')
        {
            if (i + 1 >= text.size()) { buses.clear(); return false; }
            field += text[++i];
        }
        else if (c == '/')  { if (!endField()) { buses.clear(); return false; } }
        else if (c == ';')  { if (!endBus())   { buses.clear(); return false; } }
        else                field += c;
    }
    if (!endBus()) { buses.clear(); return false; }
    return true;
}

// Fill a missing layout in with the single-main-bus reading of the flat totals.
// Used by the cache loader for entries written before layouts existed, and by
// any host whose plugin refuses to describe its buses.
inline void pluginFillDefaultBusLayout(PluginDescriptor& d)
{
    if (d.audioInBuses.empty() && d.numAudioIn > 0)
        d.audioInBuses.push_back(PluginBusInfo{ "Main", d.numAudioIn, true, false });
    if (d.audioOutBuses.empty() && d.numAudioOut > 0)
        d.audioOutBuses.push_back(PluginBusInfo{ "Main", d.numAudioOut, true, false });
}

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
    // Original transport-domain arrival sample for live input. Delivery may be
    // deferred one block for RT safety; recording must not inherit that delay.
    int64_t       captureSample = -1;
    // Tracker note-column (VOICE) this event belongs to, or -1 when untagged
    // (live MIDI in, piano-roll edits, imported files).
    //
    // The column MUST travel on the event.  It used to live in a 128-entry
    // pitch-keyed table inside the sampler, stamped ahead of time by the UI --
    // which meant the same pitch played from two columns collapsed onto one
    // column (last writer won), and with per-column mono voice allocation the
    // two notes then fought over a single voice.  Carried here, a note-on and
    // its note-off identify their voice exactly, whatever else shares the pitch.
    //
    // Appended last so positional aggregate init of the older fields stays valid.
    int8_t        column = -1;
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
    // How many channel pointers audioIn/audioOut REALLY hold.  Hosts must never
    // index past these, no matter what a plugin's own numInputs/numOutputs
    // claim (a multi-out drum plugin can declare 16 outs against our stereo
    // bus).  Defaults match the engine-wide stereo bus.  (Appended last so
    // positional aggregate init of older fields stays valid.)
    int32_t             numAudioIn  = 2;
    int32_t             numAudioOut = 2;
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

}} // namespace PatchKnob::engine

#endif
