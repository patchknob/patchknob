//----------------------------------------------------------------------------
//  seq24 Windows port - VST3 host module implementation.
//
//  Implements seq24::engine::IPluginInstance on Steinberg's VST3 SDK hosting
//  layer. See vst3_host.h and the project HOSTING_NOTES.md for rationale and
//  the proven mingw64 build recipe.
//----------------------------------------------------------------------------

// --- VST3 SDK hosting layer -------------------------------------------------
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/common/memorystream.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include "vst3_host.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace seq24 { namespace engine {

namespace {

std::string u16ToUtf8(const Vst::TChar* s)
{
    return Vst::StringConvert::convert(s);
}

// Maps a raw 3-byte MIDI controller index from a CC status byte to the VST3
// CtrlNumber used by IMidiMapping. Channel-mode and standard CCs are 0..127.
inline CtrlNumber ccToCtrlNumber(uint8_t data1) { return static_cast<CtrlNumber>(data1); }

} // namespace

//============================================================================
//  Host-side IComponentHandler: bridges plugin-UI parameter edits back into
//  our parameter model so the controller and our cached values stay in sync.
//============================================================================
class Vst3PluginInstance::Impl
{
public:
    // --- module / plugin objects --------------------------------------------
    std::shared_ptr<VST3::Hosting::Module> module;
    IPtr<PlugProvider>                     plugProvider;
    IPtr<IComponent>                       component;
    IPtr<IEditController>                   controller;
    IPtr<IAudioProcessor>                   processor;
    IPtr<IMidiMapping>                      midiMapping; // optional

    PluginDescriptor descriptor;

    // --- host context (must outlive the plugin) -----------------------------
    HostApplication hostApp;

    // --- prepared state -----------------------------------------------------
    double sampleRate   = 0.0;
    int    maxBlockSize = 0;
    bool   prepared     = false;
    bool   active       = false;

    int32 numAudioInBuses  = 0;
    int32 numAudioOutBuses = 0;
    int   eventInBusIndex  = -1;   // first event input bus (MIDI), or -1

    // --- pre-allocated realtime structures (reused every block) -------------
    HostProcessData processData;
    EventList       inputEvents{256};
    ParameterChanges inputParamChanges{64};
    ParameterChanges outputParamChanges{64};
    ProcessContext  processContext{};

    // Scratch channel-pointer arrays so process() never allocates. These point
    // at the caller's planar buffers (and at a shared silence buffer for any
    // channels the caller did not supply).
    std::vector<float*> inChanPtrs;
    std::vector<float*> outChanPtrs;
    std::vector<float>  silence;     // zero input for unconnected channels
    std::vector<float>  dump;        // discard sink for extra output channels

    // --- editor -------------------------------------------------------------
    IPtr<IPlugView> view;
    bool            editorOpen = false;

    // --- parameter cache ----------------------------------------------------
    std::vector<ParamInfo> params;

    Impl() = default;
    ~Impl() { teardown(); }

    void cacheParams();
    bool setupProcessingChain();
    void teardown();
};

void Vst3PluginInstance::Impl::cacheParams()
{
    params.clear();
    if (!controller)
        return;
    int32 n = controller->getParameterCount();
    params.reserve(static_cast<size_t>(n));
    for (int32 i = 0; i < n; ++i)
    {
        ParameterInfo pi = {};
        if (controller->getParameterInfo(i, pi) != kResultOk)
            continue;
        ParamInfo info;
        info.id           = static_cast<uint32_t>(pi.id);
        info.name         = u16ToUtf8(pi.title);
        info.defaultValue = static_cast<float>(pi.defaultNormalizedValue);
        params.push_back(std::move(info));
    }
}

bool Vst3PluginInstance::Impl::setupProcessingChain()
{
    if (!component || !processor)
        return false;

    // 1) Negotiate bus arrangements. Instruments typically have 0 audio inputs
    //    and one stereo (sometimes mono) output. Honor the plugin's reply.
    numAudioInBuses  = component->getBusCount(kAudio, kInput);
    numAudioOutBuses = component->getBusCount(kAudio, kOutput);

    std::vector<SpeakerArrangement> inArr(static_cast<size_t>(std::max(numAudioInBuses, 0)));
    std::vector<SpeakerArrangement> outArr(static_cast<size_t>(std::max(numAudioOutBuses, 0)));

    auto defaultArrFor = [&](BusDirection dir, int32 idx) -> SpeakerArrangement {
        BusInfo bi = {};
        if (component->getBusInfo(kAudio, dir, idx, bi) == kResultOk)
            return bi.channelCount >= 2 ? SpeakerArr::kStereo
                 : bi.channelCount == 1 ? SpeakerArr::kMono
                                        : SpeakerArr::kEmpty;
        return SpeakerArr::kStereo;
    };

    for (int32 i = 0; i < numAudioInBuses; ++i)
        inArr[static_cast<size_t>(i)] = defaultArrFor(kInput, i);
    for (int32 i = 0; i < numAudioOutBuses; ++i)
        outArr[static_cast<size_t>(i)] = defaultArrFor(kOutput, i);

    processor->setBusArrangements(inArr.empty() ? nullptr : inArr.data(), numAudioInBuses,
                                  outArr.empty() ? nullptr : outArr.data(), numAudioOutBuses);

    // 2) Activate all audio buses we intend to use plus the (first) MIDI bus.
    for (int32 i = 0; i < numAudioInBuses; ++i)
        component->activateBus(kAudio, kInput, i, true);
    for (int32 i = 0; i < numAudioOutBuses; ++i)
        component->activateBus(kAudio, kOutput, i, true);

    eventInBusIndex = -1;
    int32 nEvtIn = component->getBusCount(kEvent, kInput);
    if (nEvtIn > 0)
    {
        eventInBusIndex = 0;
        component->activateBus(kEvent, kInput, 0, true);
    }
    int32 nEvtOut = component->getBusCount(kEvent, kOutput);
    for (int32 i = 0; i < nEvtOut; ++i)
        component->activateBus(kEvent, kOutput, i, true);

    // 3) setupProcessing: realtime, 32-bit float.
    ProcessSetup setup = {};
    setup.processMode        = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = maxBlockSize;
    setup.sampleRate         = sampleRate;
    if (processor->setupProcessing(setup) != kResultOk)
        return false;

    // 4) Prepare the HostProcessData buffer containers. Pass bufferSamples=0 so
    //    the SDK only allocates the per-bus AudioBusBuffers arrays (and the
    //    channelBuffers32 pointer arrays) but NOT the sample storage itself: we
    //    point channelBuffers32 at the caller's planar buffers each block.
    if (!processData.prepare(*component, 0, kSample32))
        return false;
    processData.numSamples           = maxBlockSize;
    processData.symbolicSampleSize   = kSample32;
    processData.processMode          = kRealtime;
    processData.inputEvents          = &inputEvents;
    processData.inputParameterChanges  = &inputParamChanges;
    processData.outputParameterChanges = &outputParamChanges;
    processData.processContext       = &processContext;

    // Scratch buffers sized for the largest bus channel count we may touch.
    silence.assign(static_cast<size_t>(maxBlockSize), 0.0f);
    dump.assign(static_cast<size_t>(maxBlockSize), 0.0f);

    // ProcessContext defaults.
    processContext.sampleRate = sampleRate;

    return true;
}

void Vst3PluginInstance::Impl::teardown()
{
    if (view)
    {
        if (editorOpen)
            view->removed();
        view = nullptr;
        editorOpen = false;
    }
    if (processor && active)
        processor->setProcessing(false);
    if (component && active)
        component->setActive(false);
    active   = false;
    prepared = false;

    processData.unprepare();

    midiMapping = nullptr;
    processor   = nullptr;
    // PlugProvider owns/terminates component + controller; release our refs and
    // let it tear them down deterministically.
    component   = nullptr;
    controller  = nullptr;
    plugProvider = nullptr;   // terminates + disconnects component/controller
    module.reset();           // unloads the DLL
}

//============================================================================
//  Vst3PluginInstance
//============================================================================
Vst3PluginInstance::Vst3PluginInstance() : d_(new Impl()) {}
Vst3PluginInstance::~Vst3PluginInstance() = default;

bool Vst3PluginInstance::load(const PluginDescriptor& desc)
{
    d_->descriptor = desc;

    // Provide IHostApplication + IPlugInterfaceSupport to plugins during
    // initialize(). PlugProvider reads the context from PluginContextFactory.
    PluginContextFactory::instance().setPluginContext(
        static_cast<FUnknown*>(static_cast<IHostApplication*>(&d_->hostApp)));

    std::string error;
    d_->module = VST3::Hosting::Module::create(desc.path, error);
    if (!d_->module)
        return false;

    const auto& factory = d_->module->getFactory();

    // Choose the class: match by uid if given, else first audio class.
    VST3::Hosting::ClassInfo wanted;
    bool found = false;
    for (const auto& ci : factory.classInfos())
    {
        if (ci.category() != kVstAudioEffectClass)
            continue;
        if (!desc.uid.empty())
        {
            if (ci.ID().toString() == desc.uid) { wanted = ci; found = true; break; }
        }
        else if (!found)
        {
            wanted = ci; found = true; // first audio class
        }
    }
    if (!found)
        return false;

    d_->plugProvider = owned(new PlugProvider(factory, wanted, true /*plugIsGlobal*/));
    if (!d_->plugProvider->initialize())
        return false;

    d_->component  = d_->plugProvider->getComponentPtr();
    d_->controller = d_->plugProvider->getControllerPtr();
    if (!d_->component)
        return false;

    d_->processor = FUnknownPtr<IAudioProcessor>(d_->component);
    if (!d_->processor)
        return false;

    if (d_->controller)
        d_->midiMapping = FUnknownPtr<IMidiMapping>(d_->controller);

    // Sync component (processor) state -> controller so the controller's
    // parameter cache matches the DSP defaults/preset. PlugProvider connects
    // the two but does not perform this initial state transfer.
    if (d_->controller)
    {
        MemoryStream stream;
        if (d_->component->getState(&stream) == kResultOk)
        {
            stream.seek(0, IBStream::kIBSeekSet, nullptr);
            d_->controller->setComponentState(&stream);
        }
    }

    // Fill in descriptor introspection (name/buses/instrument-ness).
    if (d_->descriptor.name.empty())
        d_->descriptor.name = wanted.name();
    if (d_->descriptor.vendor.empty())
        d_->descriptor.vendor = factory.info().vendor();

    int32 nIn  = 0, nOut = 0;
    for (int32 i = 0, n = d_->component->getBusCount(kAudio, kInput); i < n; ++i)
    {
        BusInfo bi = {};
        if (d_->component->getBusInfo(kAudio, kInput, i, bi) == kResultOk)
            nIn += bi.channelCount;
    }
    for (int32 i = 0, n = d_->component->getBusCount(kAudio, kOutput); i < n; ++i)
    {
        BusInfo bi = {};
        if (d_->component->getBusInfo(kAudio, kOutput, i, bi) == kResultOk)
            nOut += bi.channelCount;
    }
    d_->descriptor.numAudioIn  = nIn;
    d_->descriptor.numAudioOut = nOut;
    d_->descriptor.format      = PluginFormat::VST3;
    d_->descriptor.isInstrument = d_->component->getBusCount(kEvent, kInput) > 0;
    if (d_->descriptor.path.empty())
        d_->descriptor.path = desc.path;

    d_->cacheParams();
    return true;
}

const PluginDescriptor& Vst3PluginInstance::descriptor() const { return d_->descriptor; }

bool Vst3PluginInstance::prepare(double sampleRate, int maxBlockSize)
{
    if (!d_->component || !d_->processor)
        return false;

    // Re-preparing requires a clean (inactive) state first.
    if (d_->active)
        setActive(false);

    d_->sampleRate   = sampleRate;
    d_->maxBlockSize = maxBlockSize;

    if (!d_->setupProcessingChain())
        return false;

    d_->prepared = true;
    return true;
}

void Vst3PluginInstance::setActive(bool active)
{
    if (!d_->component || !d_->processor || !d_->prepared)
        return;
    if (active == d_->active)
        return;

    if (active)
    {
        d_->component->setActive(true);
        d_->processor->setProcessing(true);
    }
    else
    {
        d_->processor->setProcessing(false);
        d_->component->setActive(false);
    }
    d_->active = active;
}

void Vst3PluginInstance::release() { d_->teardown(); }

void Vst3PluginInstance::process(const ProcessBlock& blk)
{
    Impl& s = *d_;
    if (!s.active || !s.processor)
    {
        // Still required to clear caller's output to avoid stale audio.
        for (int c = 0; c < s.descriptor.numAudioOut && blk.audioOut; ++c)
            if (blk.audioOut[c])
                std::memset(blk.audioOut[c], 0, sizeof(float) * static_cast<size_t>(blk.nframes));
        return;
    }

    const int32 nframes = blk.nframes;

    // --- 1. MIDI events -> EventList ---------------------------------------
    s.inputEvents.clear();
    if (s.eventInBusIndex >= 0 && blk.midiIn)
    {
        for (int32 i = 0; i < blk.numMidiIn; ++i)
        {
            const MidiEvent& m = blk.midiIn[i];
            const uint8_t statusHi = m.status & 0xF0u;
            const int16   channel  = static_cast<int16>(m.status & 0x0Fu);

            Event e = {};
            e.busIndex     = s.eventInBusIndex;
            e.sampleOffset = m.sampleOffset;
            e.flags        = Event::kIsLive;

            if (statusHi == 0x90 && m.data2 > 0) // note on (vel>0)
            {
                e.type = Event::kNoteOnEvent;
                e.noteOn.channel  = channel;
                e.noteOn.pitch    = static_cast<int16>(m.data1);
                e.noteOn.tuning   = 0.0f;
                e.noteOn.velocity = static_cast<float>(m.data2) / 127.0f;
                e.noteOn.length   = 0;
                e.noteOn.noteId   = -1;
                s.inputEvents.addEvent(e);
            }
            else if (statusHi == 0x80 || (statusHi == 0x90 && m.data2 == 0)) // note off
            {
                e.type = Event::kNoteOffEvent;
                e.noteOff.channel  = channel;
                e.noteOff.pitch    = static_cast<int16>(m.data1);
                e.noteOff.velocity = static_cast<float>(m.data2) / 127.0f;
                e.noteOff.noteId   = -1;
                e.noteOff.tuning   = 0.0f;
                s.inputEvents.addEvent(e);
            }
            else if (statusHi == 0xA0) // poly pressure
            {
                e.type = Event::kPolyPressureEvent;
                e.polyPressure.channel  = channel;
                e.polyPressure.pitch    = static_cast<int16>(m.data1);
                e.polyPressure.pressure = static_cast<float>(m.data2) / 127.0f;
                e.polyPressure.noteId   = -1;
                s.inputEvents.addEvent(e);
            }
            // CC / pitchbend / channel-pressure are routed via IMidiMapping
            // below, NOT as raw VST3 events.
        }
    }

    // --- 2. Param automation + CC->param mapping -> ParameterChanges -------
    s.inputParamChanges.clearQueue();
    s.outputParamChanges.clearQueue();

    auto addParamPoint = [&](ParamID pid, int32 offset, double norm) {
        int32 qIdx = 0;
        IParamValueQueue* q = s.inputParamChanges.addParameterData(pid, qIdx);
        if (q)
        {
            int32 ptIdx = 0;
            q->addPoint(offset, norm, ptIdx);
        }
    };

    if (blk.paramIn)
    {
        for (int32 i = 0; i < blk.numParamIn; ++i)
        {
            const ParamChange& pc = blk.paramIn[i];
            addParamPoint(static_cast<ParamID>(pc.id), pc.sampleOffset,
                          static_cast<double>(pc.value));
        }
    }

    // Map raw CC / pitchbend to parameter changes via IMidiMapping if present.
    if (s.midiMapping && s.eventInBusIndex >= 0 && blk.midiIn)
    {
        for (int32 i = 0; i < blk.numMidiIn; ++i)
        {
            const MidiEvent& m = blk.midiIn[i];
            const uint8_t statusHi = m.status & 0xF0u;
            const int16   channel  = static_cast<int16>(m.status & 0x0Fu);
            ParamID pid = 0;

            if (statusHi == 0xB0) // control change
            {
                if (s.midiMapping->getMidiControllerAssignment(
                        s.eventInBusIndex, channel, ccToCtrlNumber(m.data1), pid) == kResultOk)
                    addParamPoint(pid, m.sampleOffset, static_cast<double>(m.data2) / 127.0);
            }
            else if (statusHi == 0xE0) // pitch bend
            {
                if (s.midiMapping->getMidiControllerAssignment(
                        s.eventInBusIndex, channel, kPitchBend, pid) == kResultOk)
                {
                    const int bend = (static_cast<int>(m.data2) << 7) | static_cast<int>(m.data1);
                    addParamPoint(pid, m.sampleOffset, static_cast<double>(bend) / 16383.0);
                }
            }
            else if (statusHi == 0xD0) // channel pressure
            {
                if (s.midiMapping->getMidiControllerAssignment(
                        s.eventInBusIndex, channel, kAfterTouch, pid) == kResultOk)
                    addParamPoint(pid, m.sampleOffset, static_cast<double>(m.data1) / 127.0);
            }
        }
    }

    // --- 3. Process context -------------------------------------------------
    s.processContext.state =
        ProcessContext::kTempoValid | ProcessContext::kContTimeValid;
    if (blk.isPlaying)
        s.processContext.state |= ProcessContext::kPlaying;
    s.processContext.sampleRate          = s.sampleRate;
    s.processContext.projectTimeSamples  = static_cast<TSamples>(blk.playPositionSamples);
    s.processContext.continousTimeSamples = static_cast<TSamples>(blk.playPositionSamples);
    s.processContext.tempo               = blk.tempoBpm;
    s.processContext.timeSigNumerator    = 4;
    s.processContext.timeSigDenominator  = 4;

    // --- 4. Wire planar audio buffers into the HostProcessData -------------
    s.processData.numSamples = nframes;

    // Inputs: point each channel of each input bus at the caller's buffer when
    // available, else at the shared silence buffer.
    int inChanCursor = 0;
    for (int32 b = 0; b < s.processData.numInputs; ++b)
    {
        AudioBusBuffers& bus = s.processData.inputs[b];
        bus.silenceFlags = 0;
        for (int32 c = 0; c < bus.numChannels; ++c)
        {
            float* p = nullptr;
            if (blk.audioIn && inChanCursor < s.descriptor.numAudioIn)
                p = const_cast<float*>(blk.audioIn[inChanCursor]);
            if (!p)
            {
                p = s.silence.data();
                bus.silenceFlags |= (uint64(1) << c);
            }
            bus.channelBuffers32[c] = p;
            ++inChanCursor;
        }
    }

    // Outputs: route the first numAudioOut channels to the caller; any extra
    // channels go to the discard buffer so the plugin always has valid storage.
    int outChanCursor = 0;
    for (int32 b = 0; b < s.processData.numOutputs; ++b)
    {
        AudioBusBuffers& bus = s.processData.outputs[b];
        bus.silenceFlags = 0;
        for (int32 c = 0; c < bus.numChannels; ++c)
        {
            float* p = nullptr;
            if (blk.audioOut && outChanCursor < s.descriptor.numAudioOut)
                p = blk.audioOut[outChanCursor];
            if (!p)
                p = s.dump.data();
            bus.channelBuffers32[c] = p;
            ++outChanCursor;
        }
    }

    // --- 5. Process ---------------------------------------------------------
    s.processData.inputEvents          = &s.inputEvents;
    s.processData.inputParameterChanges  = &s.inputParamChanges;
    s.processData.outputParameterChanges = &s.outputParamChanges;
    s.processData.processContext       = &s.processContext;

    s.processor->process(s.processData);

    // --- 6. Drain output parameter changes into the controller --------------
    // (Keeps our controller-side cache consistent with plugin-driven moves.)
    if (s.controller)
    {
        int32 nq = s.outputParamChanges.getParameterCount();
        for (int32 i = 0; i < nq; ++i)
        {
            IParamValueQueue* q = s.outputParamChanges.getParameterData(i);
            if (!q) continue;
            int32 pts = q->getPointCount();
            if (pts <= 0) continue;
            int32 off = 0; ParamValue val = 0.0;
            if (q->getPoint(pts - 1, off, val) == kResultOk)
                s.controller->setParamNormalized(q->getParameterId(), val);
        }
    }
}

int Vst3PluginInstance::paramCount() const { return static_cast<int>(d_->params.size()); }

ParamInfo Vst3PluginInstance::paramInfo(int index) const
{
    if (index < 0 || index >= static_cast<int>(d_->params.size()))
        return ParamInfo{};
    return d_->params[static_cast<size_t>(index)];
}

float Vst3PluginInstance::getParamNormalized(uint32_t id) const
{
    if (!d_->controller)
        return 0.0f;
    return static_cast<float>(d_->controller->getParamNormalized(static_cast<ParamID>(id)));
}

void Vst3PluginInstance::setParamNormalized(uint32_t id, float v)
{
    if (!d_->controller)
        return;
    // Update the controller (UI) side. The processor side is driven through
    // ParamChange[] in process(); see plugin_api.h contract.
    d_->controller->setParamNormalized(static_cast<ParamID>(id), v);
}

bool Vst3PluginInstance::hasEditor() const
{
    if (!d_->controller)
        return false;
    if (d_->view)
        return true;
    IPtr<IPlugView> v = owned(d_->controller->createView(ViewType::kEditor));
    if (!v)
        return false;
    return v->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue;
}

bool Vst3PluginInstance::openEditor(NativeWindowHandle parent)
{
    if (!d_->controller)
        return false;
    if (d_->editorOpen)
        return true;
    if (!d_->view)
        d_->view = owned(d_->controller->createView(ViewType::kEditor));
    if (!d_->view)
        return false;
    if (d_->view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue)
        return false;
    if (d_->view->attached(parent, kPlatformTypeHWND) != kResultOk)
        return false;
    d_->editorOpen = true;
    return true;
}

void Vst3PluginInstance::closeEditor()
{
    if (d_->view && d_->editorOpen)
        d_->view->removed();
    d_->editorOpen = false;
}

void Vst3PluginInstance::getEditorSize(int& w, int& h) const
{
    w = 0; h = 0;
    if (!d_->view)
        return;
    ViewRect r = {};
    if (d_->view->getSize(&r) == kResultOk)
    {
        w = r.right - r.left;
        h = r.bottom - r.top;
    }
}

void Vst3PluginInstance::idleEditor()
{
    // VST3 editors are driven by the Win32 message loop / IRunLoop; no explicit
    // idle pump is required (unlike VST2's effEditIdle). Intentionally empty.
}

std::vector<uint8_t> Vst3PluginInstance::saveState() const
{
    std::vector<uint8_t> out;
    if (!d_->component)
        return out;

    // Layout: [uint32 compSize][comp bytes][uint32 ctrlSize][ctrl bytes].
    auto appendBlob = [&](MemoryStream& ms) {
        const uint32_t n = static_cast<uint32_t>(ms.getSize());
        const uint8_t* p = reinterpret_cast<const uint8_t*>(ms.getData());
        out.push_back(static_cast<uint8_t>(n & 0xFF));
        out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((n >> 24) & 0xFF));
        out.insert(out.end(), p, p + n);
    };

    MemoryStream compStream;
    d_->component->getState(&compStream);
    appendBlob(compStream);

    MemoryStream ctrlStream;
    if (d_->controller)
        d_->controller->getState(&ctrlStream);
    appendBlob(ctrlStream);

    return out;
}

void Vst3PluginInstance::loadState(const std::vector<uint8_t>& data)
{
    if (!d_->component || data.size() < 4)
        return;

    size_t pos = 0;
    auto readBlob = [&](std::vector<uint8_t>& dst) -> bool {
        if (pos + 4 > data.size()) return false;
        uint32_t n = static_cast<uint32_t>(data[pos])
                   | (static_cast<uint32_t>(data[pos + 1]) << 8)
                   | (static_cast<uint32_t>(data[pos + 2]) << 16)
                   | (static_cast<uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        if (pos + n > data.size()) return false;
        dst.assign(data.begin() + static_cast<long>(pos),
                   data.begin() + static_cast<long>(pos + n));
        pos += n;
        return true;
    };

    std::vector<uint8_t> compBytes, ctrlBytes;
    if (!readBlob(compBytes))
        return;
    readBlob(ctrlBytes); // controller blob is optional

    // Restore: component->setState, controller->setComponentState (component
    // blob), then controller->setState (controller blob). Order matters.
    {
        MemoryStream ms(compBytes.data(), static_cast<TSize>(compBytes.size()));
        d_->component->setState(&ms);
    }
    if (d_->controller && !compBytes.empty())
    {
        MemoryStream ms(compBytes.data(), static_cast<TSize>(compBytes.size()));
        d_->controller->setComponentState(&ms);
    }
    if (d_->controller && !ctrlBytes.empty())
    {
        MemoryStream ms(ctrlBytes.data(), static_cast<TSize>(ctrlBytes.size()));
        d_->controller->setState(&ms);
    }
}

//============================================================================
//  Factory
//============================================================================
IPluginInstance* createVst3Instance(const PluginDescriptor& desc)
{
    auto* inst = new Vst3PluginInstance();
    if (!inst->load(desc))
    {
        delete inst;
        return nullptr;
    }
    return inst;
}

}} // namespace seq24::engine
