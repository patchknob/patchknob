//----------------------------------------------------------------------------
//  seq24 Windows port (ZERO-JUCE) — VST2 host implementation.
//  See vst2_host.h for the contract. Uses the clean-room vestige aeffectx.h.
//----------------------------------------------------------------------------
#include "vst2_host.h"

#include "aeffectx.h"

#include <windows.h>

#include <cstring>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace seq24 { namespace engine {

// ---------------------------------------------------------------------------
// Opcodes the clean-room header does not declare but that we use. These are
// published VST2 interface constants (numeric values), not Steinberg source.
// ---------------------------------------------------------------------------
#ifndef effGetNumProgramCategories
static constexpr int kEffGetNumProgramCategories = 7;
#endif
static constexpr int kEffCanBeAutomated = 26;
static constexpr int kEffString2Parameter = 27;

// AEffect flag for "uses opaque chunk for state" (effFlagsProgramChunks).
static constexpr int kEffFlagsProgramChunks = 1 << 5;

// ERect returned by effEditGetRect (the clean-room header omits the struct).
struct ERect { int16_t top, left, bottom, right; };

// Plugin DLL entry point: AEffect* entry(audioMasterCallback host).
typedef AEffect* (VST_CALL_CONV* VstEntryProc)(audioMasterCallback);

// While a plugin is being constructed (entry() / effOpen), it may issue host
// callbacks before we have a chance to stash `this` in AEffect::user. This
// thread-local points at the instance currently performing load() so those
// early callbacks (audioMasterGetSampleRate/GetBlockSize/GetTime/...) reach
// the right host instead of silently returning 0. (Plugin construction is
// single-threaded on the message thread, so thread-local is sufficient.)
static thread_local Vst2PluginInstance* g_loadingInstance = nullptr;

// Registry mapping AEffect* -> owning Vst2PluginInstance*. We do NOT stash the
// host pointer in AEffect::user, because the exact byte offset of `user` in the
// clean-room header is unverified for x64 and writing the wrong slot corrupts
// the plugin (observed: total silence from otherwise-working synths). A side
// table keyed on the AEffect* (which we read, not write) is offset-independent.
static std::mutex& registryMutex()
{
    static std::mutex m;
    return m;
}
static std::unordered_map<AEffect*, Vst2PluginInstance*>& registry()
{
    static std::unordered_map<AEffect*, Vst2PluginInstance*> r;
    return r;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
Vst2PluginInstance::Vst2PluginInstance()
{
    timeInfo_ = new VstTimeInfo();
    std::memset(timeInfo_, 0, sizeof(VstTimeInfo));
}

Vst2PluginInstance::~Vst2PluginInstance()
{
    release();
    delete timeInfo_;
    timeInfo_ = nullptr;
}

// ---------------------------------------------------------------------------
// dispatcher convenience
// ---------------------------------------------------------------------------
intptr_t Vst2PluginInstance::dispatch(int32_t opcode, int32_t index,
                                      intptr_t value, void* ptr, float opt) const
{
    if (!effect_) return 0;
    return effect_->dispatcher(effect_, opcode, index, value, ptr, opt);
}

// ---------------------------------------------------------------------------
// Host callback — the plugin calls this to query host services. We stash
// `this` in AEffect::user before calling effOpen so we can trampoline.
// ---------------------------------------------------------------------------
intptr_t VST_CALL_CONV Vst2PluginInstance::hostCallbackStatic(
    AEffect* effect, int32_t opcode, int32_t index, intptr_t value,
    void* ptr, float opt)
{
    // During the entry() call (before we can store `this`), opcodes like
    // audioMasterVersion arrive with effect==null or user==null. Answer the
    // load-time essentials without an instance.
    Vst2PluginInstance* self = nullptr;
    if (effect)
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        auto it = registry().find(effect);
        if (it != registry().end()) self = it->second;
    }
    if (!self)
        self = g_loadingInstance;  // during entry()/effOpen, before registration

    if (!self)
    {
        // Truly no context (shouldn't happen) — answer the load essentials.
        switch (opcode)
        {
        case audioMasterVersion:   return 2400;
        case audioMasterCurrentId: return 0;
        default:                   return 0;
        }
    }
    return self->hostCallbackImpl(opcode, index, value, ptr, opt);
}

intptr_t Vst2PluginInstance::hostCallbackImpl(int32_t opcode, int32_t index,
                                              intptr_t value, void* ptr,
                                              float opt)
{
    (void)index; (void)value; (void)opt;

    switch (opcode)
    {
    case audioMasterVersion:
        return 2400;                      // VST 2.4

    case audioMasterCurrentId:
        return 0;                         // first/default sub-plugin

    case audioMasterGetSampleRate:
        return (intptr_t)sampleRate_;

    case audioMasterGetBlockSize:
        return (intptr_t)maxBlockSize_;

    case audioMasterGetVendorString:
        if (ptr) std::strcpy((char*)ptr, "seq24");
        return 1;

    case audioMasterGetProductString:
        if (ptr) std::strcpy((char*)ptr, "seq24 VST2 host");
        return 1;

    case audioMasterGetVendorVersion:
        return 1;

    case audioMasterGetLanguage:
        return kVstLangEnglish;

    case audioMasterCanDo:
        if (ptr)
        {
            const char* s = (const char*)ptr;
            if (std::strcmp(s, "sendVstMidiEvent") == 0)    return 1;
            if (std::strcmp(s, "receiveVstMidiEvent") == 0) return 1;
            if (std::strcmp(s, "sizeWindow") == 0)          return 1;
            if (std::strcmp(s, "sendVstTimeInfo") == 0)     return 1;
        }
        return 0;

    case audioMasterGetCurrentProcessLevel:
        // 2 == realtime audio thread, 1 == GUI/user thread. We report realtime
        // while active+prepared, otherwise user level.
        return (active_ && prepared_) ? 2 : 1;

    case audioMasterGetTime:
    {
        // Fill our owned VstTimeInfo from the current block snapshot.
        std::memset(timeInfo_, 0, sizeof(VstTimeInfo));
        timeInfo_->samplePos  = (double)curPlayPos_;
        timeInfo_->sampleRate = sampleRate_;
        timeInfo_->tempo      = curTempo_ > 0.0 ? curTempo_ : 120.0;
        // ppq position = quarter notes elapsed = (samples / sr) * (bpm / 60).
        double secs = (sampleRate_ > 0.0) ? (double)curPlayPos_ / sampleRate_ : 0.0;
        timeInfo_->ppqPos = secs * (timeInfo_->tempo / 60.0);
        timeInfo_->timeSigNumerator   = 4;
        timeInfo_->timeSigDenominator = 4;
        // barStartPos: ppq at the start of the current bar (4 quarters/bar).
        timeInfo_->barStartPos =
            std::floor(timeInfo_->ppqPos / 4.0) * 4.0;
        int32_t flags = kVstTempoValid | kVstPpqPosValid | kVstBarsValid |
                        kVstTimeSigValid;
        if (curPlaying_) flags |= kVstTransportPlaying;
        timeInfo_->flags = flags;
        return (intptr_t)timeInfo_;
    }

    case audioMasterAutomate:
        // Plugin's GUI moved a parameter. A full host records this for
        // automation; here we simply acknowledge.
        return 0;

    case audioMasterIdle:
    case audioMasterNeedIdle:
        // Acknowledge; the message-thread idleEditor() pump drives effEditIdle.
        // (Do NOT dispatch effEditIdle from here — the plugin may call this
        // during processReplacing, and re-entering the dispatcher is unsafe.)
        return 1;

    case audioMasterSizeWindow:
        // index = width, value = height requested by the plugin.
        return 1;

    case audioMasterIOChanged:
        if (effect_)
        {
            numIn_  = effect_->numInputs;
            numOut_ = effect_->numOutputs;
        }
        return 1;

    case audioMasterWantMidi:
    case audioMasterUpdateDisplay:
    case audioMasterBeginEdit:
    case audioMasterEndEdit:
    case audioMasterPinConnected:
        return 0;

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// load — LoadLibrary + resolve entry + instantiate AEffect + effOpen.
// ---------------------------------------------------------------------------
bool Vst2PluginInstance::load(const PluginDescriptor& desc)
{
    desc_ = desc;

    // LOAD_WITH_ALTERED_SEARCH_PATH so the plugin finds sibling resource DLLs
    // relative to its own directory.
    HMODULE mod = LoadLibraryExA(desc.path.c_str(), nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod)
        mod = LoadLibraryA(desc.path.c_str());
    if (!mod)
        return false;
    module_ = mod;

    VstEntryProc entry =
        (VstEntryProc)(void*)GetProcAddress(mod, "VSTPluginMain");
    if (!entry)
        entry = (VstEntryProc)(void*)GetProcAddress(mod, "main");
    if (!entry)
    {
        FreeLibrary(mod);
        module_ = nullptr;
        return false;
    }

    // Route early callbacks (during entry()/effOpen) to this instance.
    g_loadingInstance = this;
    AEffect* eff = entry(&Vst2PluginInstance::hostCallbackStatic);
    if (!eff || eff->magic != kEffectMagic)
    {
        g_loadingInstance = nullptr;
        FreeLibrary(mod);
        module_ = nullptr;
        return false;
    }
    effect_ = eff;
    // Register so the trampoline can map this AEffect* back to us, WITHOUT
    // writing into the AEffect struct (offset of `user` is unverified for x64).
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        registry()[effect_] = this;
    }

    dispatch(effOpen, 0, 0, nullptr, 0.0f);
    opened_ = true;
    g_loadingInstance = nullptr;

    numIn_  = effect_->numInputs;
    numOut_ = effect_->numOutputs;

    // Fill in descriptor fields we can now read from the plugin.
    char buf[512];
    std::memset(buf, 0, sizeof(buf));
    if (dispatch(effGetEffectName, 0, 0, buf, 0.0f) && buf[0])
        desc_.name = buf;
    std::memset(buf, 0, sizeof(buf));
    if (dispatch(effGetVendorString, 0, 0, buf, 0.0f) && buf[0])
        desc_.vendor = buf;
    desc_.format       = PluginFormat::VST2;
    desc_.numAudioIn   = numIn_;
    desc_.numAudioOut  = numOut_;
    desc_.isInstrument = (effect_->flags & effFlagsIsSynth) != 0;

    return true;
}

// ---------------------------------------------------------------------------
// prepare — set sample rate / block size, allocate RT scratch, activate.
// ---------------------------------------------------------------------------
bool Vst2PluginInstance::prepare(double sampleRate, int maxBlockSize)
{
    if (!effect_) return false;

    sampleRate_   = sampleRate;
    maxBlockSize_ = maxBlockSize;

    dispatch(effSetSampleRate, 0, 0, nullptr, (float)sampleRate_);
    dispatch(effSetBlockSize, 0, (intptr_t)maxBlockSize_, nullptr, 0.0f);

    allocChannelBuffers();

    // Pre-allocate a MIDI event buffer big enough for a generous block.
    // (freeEventBuffer() resets maxEvents_ to 0, so set the count AFTER it.)
    freeEventBuffer();
    maxEvents_ = 1024;
    // VstEvents has a trailing events[1]; over-allocate for maxEvents_ ptrs.
    size_t evBytes = sizeof(VstEvents) +
                     (size_t)(maxEvents_ - 1) * sizeof(VstEvent*);
    vstEvents_ = (VstEvents*)std::malloc(evBytes);
    std::memset(vstEvents_, 0, evBytes);
    eventStorage_ = std::malloc((size_t)maxEvents_ * sizeof(VstMidiEvent));
    std::memset(eventStorage_, 0, (size_t)maxEvents_ * sizeof(VstMidiEvent));

    prepared_ = true;

    // Turn the plugin on (resume).
    setActive(true);
    return true;
}

void Vst2PluginInstance::allocChannelBuffers()
{
    inPtrs_.assign((size_t)std::max(numIn_, 0), nullptr);
    outPtrs_.assign((size_t)std::max(numOut_, 0), nullptr);

    // A zeroed scratch block used for any input channel the caller doesn't
    // supply (synths report 0 inputs but we still pass valid pointers).
    silence_.assign((size_t)maxBlockSize_, 0.0f);

    // Contiguous storage for plugin inputs we synthesize when caller passes
    // null audioIn. One block per input channel.
    inStorage_.assign((size_t)numIn_ * (size_t)maxBlockSize_, 0.0f);
}

void Vst2PluginInstance::freeChannelBuffers()
{
    inPtrs_.clear();
    outPtrs_.clear();
    inStorage_.clear();
    silence_.clear();
}

void Vst2PluginInstance::freeEventBuffer()
{
    if (vstEvents_)   { std::free(vstEvents_);    vstEvents_   = nullptr; }
    if (eventStorage_){ std::free(eventStorage_); eventStorage_ = nullptr; }
    maxEvents_ = 0;
}

// ---------------------------------------------------------------------------
// setActive — effMainsChanged toggles resume(1)/suspend(0).
// ---------------------------------------------------------------------------
void Vst2PluginInstance::setActive(bool active)
{
    if (!effect_) return;
    if (active == active_) return;
    dispatch(effMainsChanged, 0, active ? 1 : 0, nullptr, 0.0f);
    active_ = active;
}

// ---------------------------------------------------------------------------
// release — suspend, close, free library and buffers. Idempotent.
// ---------------------------------------------------------------------------
void Vst2PluginInstance::release()
{
    if (effect_)
    {
        if (active_)
        {
            dispatch(effMainsChanged, 0, 0, nullptr, 0.0f);
            active_ = false;
        }
        if (opened_)
        {
            dispatch(effClose, 0, 0, nullptr, 0.0f);
            opened_ = false;
        }
        // effClose frees the AEffect; do not touch it afterwards.
        {
            std::lock_guard<std::mutex> lk(registryMutex());
            registry().erase(effect_);
        }
        effect_ = nullptr;
    }
    if (module_)
    {
        FreeLibrary((HMODULE)module_);
        module_ = nullptr;
    }
    freeChannelBuffers();
    freeEventBuffer();
    prepared_ = false;
}

// ---------------------------------------------------------------------------
// process — RT: apply param changes, deliver MIDI, processReplacing.
// ---------------------------------------------------------------------------
void Vst2PluginInstance::process(const ProcessBlock& blk)
{
    if (!effect_ || !prepared_) return;

    const int n = blk.nframes;
    if (n <= 0 || n > maxBlockSize_) return;

    // Snapshot transport for audioMasterGetTime (read by plugin during process).
    curTempo_   = blk.tempoBpm > 0.0 ? blk.tempoBpm : 120.0;
    curPlayPos_ = blk.playPositionSamples;
    curPlaying_ = blk.isPlaying;

    // --- parameter automation (block-start, not sample-accurate for v1) ----
    for (int i = 0; i < blk.numParamIn; ++i)
    {
        const ParamChange& pc = blk.paramIn[i];
        if ((int)pc.id < effect_->numParams)
            effect_->setParameter(effect_, (int32_t)pc.id, pc.value);
    }

    // --- MIDI delivery ------------------------------------------------------
    if (blk.numMidiIn > 0 && vstEvents_ && eventStorage_)
    {
        int count = blk.numMidiIn;
        if (count > maxEvents_) count = maxEvents_;
        VstMidiEvent* evs = (VstMidiEvent*)eventStorage_;
        for (int i = 0; i < count; ++i)
        {
            const MidiEvent& m = blk.midiIn[i];
            VstMidiEvent& e = evs[i];
            std::memset(&e, 0, sizeof(VstMidiEvent));
            e.type        = kVstMidiType;
            e.byteSize    = sizeof(VstMidiEvent);
            e.deltaFrames = (m.sampleOffset >= 0 && m.sampleOffset < n)
                                ? m.sampleOffset : 0;
            e.midiData[0] = (char)m.status;
            e.midiData[1] = (char)m.data1;
            e.midiData[2] = (char)m.data2;
            e.midiData[3] = 0;
            vstEvents_->events[i] = (VstEvent*)&e;
        }
        vstEvents_->numEvents = count;
        vstEvents_->reserved  = nullptr;
        dispatch(effProcessEvents, 0, 0, vstEvents_, 0.0f);
    }

    // --- build input pointer array -----------------------------------------
    for (int c = 0; c < numIn_; ++c)
    {
        if (blk.audioIn && blk.audioIn[c])
            inPtrs_[(size_t)c] = const_cast<float*>(blk.audioIn[c]);
        else
        {
            // Use (and clear) our own scratch block as silent input.
            float* p = inStorage_.data() + (size_t)c * (size_t)maxBlockSize_;
            std::memset(p, 0, (size_t)n * sizeof(float));
            inPtrs_[(size_t)c] = p;
        }
    }

    // --- output pointer array (caller owns the buffers) ---------------------
    for (int c = 0; c < numOut_; ++c)
        outPtrs_[(size_t)c] = blk.audioOut ? blk.audioOut[c] : nullptr;

    float** ins  = numIn_  > 0 ? inPtrs_.data()  : nullptr;
    float** outs = numOut_ > 0 ? outPtrs_.data() : nullptr;

    if (effect_->processReplacing)
        effect_->processReplacing(effect_, ins, outs, n);
}

// ---------------------------------------------------------------------------
// parameters
// ---------------------------------------------------------------------------
int Vst2PluginInstance::paramCount() const
{
    return effect_ ? effect_->numParams : 0;
}

ParamInfo Vst2PluginInstance::paramInfo(int index) const
{
    ParamInfo info;
    info.id = (uint32_t)index;
    info.defaultValue = 0.0f;
    info.name.clear();
    if (effect_ && index >= 0 && index < effect_->numParams)
    {
        char buf[256] = {0};
        dispatch(effGetParamName, index, 0, buf, 0.0f);
        info.name = buf;
        info.defaultValue = effect_->getParameter(effect_, index);
    }
    return info;
}

float Vst2PluginInstance::getParamNormalized(uint32_t id) const
{
    if (effect_ && (int)id < effect_->numParams)
        return effect_->getParameter(effect_, (int32_t)id);
    return 0.0f;
}

void Vst2PluginInstance::setParamNormalized(uint32_t id, float v)
{
    if (effect_ && (int)id < effect_->numParams)
        effect_->setParameter(effect_, (int32_t)id, v);
}

// ---------------------------------------------------------------------------
// editor
// ---------------------------------------------------------------------------
bool Vst2PluginInstance::hasEditor() const
{
    return effect_ && (effect_->flags & effFlagsHasEditor);
}

bool Vst2PluginInstance::openEditor(NativeWindowHandle parent)
{
    if (!hasEditor()) return false;
    // effEditOpen returns nonzero on success for most plugins, but some return
    // 0 yet still parent correctly. Treat a successful dispatch (and a valid
    // parent) as success.
    dispatch(effEditOpen, 0, 0, parent, 0.0f);
    return parent != nullptr;
}

void Vst2PluginInstance::closeEditor()
{
    if (hasEditor())
        dispatch(effEditClose, 0, 0, nullptr, 0.0f);
}

void Vst2PluginInstance::getEditorSize(int& w, int& h) const
{
    w = 0; h = 0;
    if (!hasEditor()) return;
    ERect* r = nullptr;
    dispatch(effEditGetRect, 0, 0, &r, 0.0f);
    if (r)
    {
        w = r->right - r->left;
        h = r->bottom - r->top;
    }
}

void Vst2PluginInstance::idleEditor()
{
    if (hasEditor())
        dispatch(effEditIdle, 0, 0, nullptr, 0.0f);
}

// ---------------------------------------------------------------------------
// state (chunk if supported, else per-parameter fallback)
// ---------------------------------------------------------------------------
std::vector<uint8_t> Vst2PluginInstance::saveState() const
{
    std::vector<uint8_t> out;
    if (!effect_) return out;

    if (effect_->flags & kEffFlagsProgramChunks)
    {
        void* chunk = nullptr;
        // index 0 == bank (whole plugin) chunk.
        intptr_t size = dispatch(effGetChunk, 0, 0, &chunk, 0.0f);
        if (chunk && size > 0)
        {
            out.resize((size_t)size);
            std::memcpy(out.data(), chunk, (size_t)size);
            return out;
        }
    }

    // Fallback: serialize every parameter value as a little-endian float.
    int np = effect_->numParams;
    out.resize((size_t)np * sizeof(float));
    for (int i = 0; i < np; ++i)
    {
        float v = effect_->getParameter(effect_, i);
        std::memcpy(out.data() + (size_t)i * sizeof(float), &v, sizeof(float));
    }
    return out;
}

void Vst2PluginInstance::loadState(const std::vector<uint8_t>& data)
{
    if (!effect_ || data.empty()) return;

    if (effect_->flags & kEffFlagsProgramChunks)
    {
        dispatch(effSetChunk, 0, (intptr_t)data.size(),
                 (void*)data.data(), 0.0f);
        return;
    }

    // Fallback: per-parameter floats.
    int np = effect_->numParams;
    size_t have = data.size() / sizeof(float);
    int count = (int)std::min((size_t)np, have);
    for (int i = 0; i < count; ++i)
    {
        float v;
        std::memcpy(&v, data.data() + (size_t)i * sizeof(float), sizeof(float));
        effect_->setParameter(effect_, i, v);
    }
}

// ---------------------------------------------------------------------------
// factory
// ---------------------------------------------------------------------------
IPluginInstance* createVst2Instance(const PluginDescriptor& desc)
{
    Vst2PluginInstance* inst = new Vst2PluginInstance();
    if (!inst->load(desc))
    {
        delete inst;
        return nullptr;
    }
    return inst;
}

}} // namespace seq24::engine
