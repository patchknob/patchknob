//----------------------------------------------------------------------------
//  PatchKnob (ZERO-JUCE) — VST2 host implementation.
//  See vst2_host.h for the contract. Uses the clean-room vestige aeffectx.h.
//----------------------------------------------------------------------------
#include "vst2_host.h"

#include "aeffectx.h"
#include "seh_guard.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace PatchKnob { namespace engine {

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

// VST2 host string buffer limits (the clean-room header omits the constants;
// the spec guarantees the plugin only 64 bytes for vendor/product strings).
static constexpr size_t kVstMaxVendorStrLen  = 64;
static constexpr size_t kVstMaxProductStrLen = 64;

// Load-time sanity bounds for the plugin's self-reported counts. Anything
// outside these is a malformed/hostile AEffect and the plugin is refused
// rather than allowed to size buffers (or index arrays) with garbage.
static constexpr int kMaxPluginChannels = 64;
static constexpr int kMaxPluginParams   = 100000;

// Bounded string copy: never writes more than `cap` bytes, always terminates.
static void copyBounded(char* dst, const char* src, size_t cap)
{
    if (!dst || cap == 0) return;
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// ---------------------------------------------------------------------------
// seh_guarded_call thunks — every raw call into the plugin (dispatcher,
// processReplacing, set/getParameter, the DLL entry point) is routed through
// exactly one of these so a fault inside the plugin is contained and the
// instance can degrade to silence instead of killing the app.
// ---------------------------------------------------------------------------
struct DispatchCall {
    AEffect* eff;
    int32_t  opcode;
    int32_t  index;
    intptr_t value;
    void*    ptr;
    float    opt;
    intptr_t result;
};
static void runDispatchCall(void* p)
{
    DispatchCall* c = (DispatchCall*)p;
    c->result = c->eff->dispatcher(c->eff, c->opcode, c->index, c->value,
                                   c->ptr, c->opt);
}

struct ProcessCall {
    AEffect* eff;
    float**  ins;
    float**  outs;
    int32_t  nframes;
    bool     replacing;
};
static void runProcessCall(void* p)
{
    ProcessCall* c = (ProcessCall*)p;
    if (c->replacing) c->eff->processReplacing(c->eff, c->ins, c->outs, c->nframes);
    else              c->eff->process(c->eff, c->ins, c->outs, c->nframes);
}

struct SetParamCall {
    AEffect* eff;
    int32_t  index;
    float    value;
};
static void runSetParamCall(void* p)
{
    SetParamCall* c = (SetParamCall*)p;
    c->eff->setParameter(c->eff, c->index, c->value);
}

struct GetParamCall {
    AEffect* eff;
    int32_t  index;
    float    result;
};
static void runGetParamCall(void* p)
{
    GetParamCall* c = (GetParamCall*)p;
    c->result = c->eff->getParameter(c->eff, c->index);
}

// ERect returned by effEditGetRect (the clean-room header omits the struct).
struct ERect { int16_t top, left, bottom, right; };

// Plugin DLL entry point: AEffect* entry(audioMasterCallback host).
typedef AEffect* (VST_CALL_CONV* VstEntryProc)(audioMasterCallback);

// Guarded thunk for the DLL entry call itself — a fault while the plugin
// constructs would otherwise kill the app before load() can even fail.
struct EntryCall {
    VstEntryProc        entry;
    audioMasterCallback host;
    AEffect*            result;
};
static void runEntryCall(void* p)
{
    EntryCall* c = (EntryCall*)p;
    c->result = c->entry(c->host);
}

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
// dispatcher convenience — every opcode goes through ONE fault-guarded
// helper. A fault inside any dispatcher call (effOpen, effProcessEvents,
// effSetChunk, ...) latches dead_ and returns a safe 0 instead of crashing;
// a dead instance no-ops all further dispatch.
// ---------------------------------------------------------------------------
intptr_t Vst2PluginInstance::dispatch(int32_t opcode, int32_t index,
                                      intptr_t value, void* ptr, float opt) const
{
    if (!effect_ || dead_.load(std::memory_order_relaxed)) return 0;
    DispatchCall call{ effect_, opcode, index, value, ptr, opt, 0 };
    uint32_t code = 0;
    if (!seh_guarded_call(&runDispatchCall, &call, &code))
    {
        markDead("dispatcher", code);
        return 0;
    }
    return call.result;
}

// ---------------------------------------------------------------------------
// fault bookkeeping + guarded raw parameter calls
// ---------------------------------------------------------------------------
void Vst2PluginInstance::markDead(const char* where, uint32_t code) const
{
    // First fault wins; later calls on a dead instance are already no-ops.
    if (dead_.exchange(true, std::memory_order_acq_rel)) return;
    std::fprintf(stderr,
                 "vst2: plugin '%s' faulted in %s (code 0x%08X) — instance "
                 "disabled, output silenced\n",
                 desc_.name.empty() ? desc_.path.c_str() : desc_.name.c_str(),
                 where, (unsigned)code);
}

void Vst2PluginInstance::guardedSetParameter(int32_t index, float value) const
{
    if (!effect_ || !effect_->setParameter ||
        dead_.load(std::memory_order_relaxed))
        return;
    SetParamCall call{ effect_, index, value };
    uint32_t code = 0;
    if (!seh_guarded_call(&runSetParamCall, &call, &code))
        markDead("setParameter", code);
}

float Vst2PluginInstance::guardedGetParameter(int32_t index) const
{
    if (!effect_ || !effect_->getParameter ||
        dead_.load(std::memory_order_relaxed))
        return 0.0f;
    GetParamCall call{ effect_, index, 0.0f };
    uint32_t code = 0;
    if (!seh_guarded_call(&runGetParamCall, &call, &code))
    {
        markDead("getParameter", code);
        return 0.0f;
    }
    return call.result;
}

// ---------------------------------------------------------------------------
// teardown gate (message-thread half) — flip alive_ off, then wait until the
// audio thread's in-flight process() block (if any) has drained. process()
// raises processing_ FIRST and re-checks alive_, so once processing_ reads
// false here no new block can be inside the plugin.
// ---------------------------------------------------------------------------
void Vst2PluginInstance::quiesceProcessing()
{
    alive_.store(false, std::memory_order_seq_cst);
    while (processing_.load(std::memory_order_acquire))
        Sleep(0);   // an audio block is a few ms at most
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
        // The spec guarantees the plugin's buffer only kVstMaxVendorStrLen
        // bytes — copy bounded, never strcpy into plugin memory.
        if (ptr) copyBounded((char*)ptr, "PatchKnob", kVstMaxVendorStrLen);
        return 1;

    case audioMasterGetProductString:
        if (ptr) copyBounded((char*)ptr, "PatchKnob VST2 host", kVstMaxProductStrLen);
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
        // A plugin may re-report channel counts after a preset load, but the
        // RT scratch (inPtrs_/outPtrs_/inStorage_/dump_) was sized once at
        // prepare() and MAY be in use by a concurrent process(). Accepting
        // new counts live would let process() index past those allocations
        // (heap corruption), so we deliberately decline: the plugin keeps
        // its prepare()-time channel layout until the host re-prepares it.
        return 0;

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
    EntryCall entryCall{ entry, &Vst2PluginInstance::hostCallbackStatic,
                         nullptr };
    uint32_t entryFault = 0;
    if (!seh_guarded_call(&runEntryCall, &entryCall, &entryFault))
    {
        // The plugin faulted while constructing. Its DLL state is unknown,
        // so deliberately LEAK the module (unloading could fault again in
        // DllMain) and refuse the plugin.
        g_loadingInstance = nullptr;
        module_ = nullptr;
        std::fprintf(stderr,
                     "vst2: '%s' faulted in its entry point (code 0x%08X) — "
                     "refusing to load\n",
                     desc.path.c_str(), (unsigned)entryFault);
        return false;
    }
    AEffect* eff = entryCall.result;
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

    // A fault inside effOpen latched dead_; the instance is unusable.
    if (dead_.load(std::memory_order_relaxed))
    {
        release();
        return false;
    }

    // Refuse plugins whose self-reported counts are insane BEFORE those
    // counts size any buffer or bound any loop (a negative numInputs wraps
    // (size_t) casts to huge allocations; absurd numOutputs would make
    // process() walk garbage).
    if (!validateEffectCounts())
    {
        std::fprintf(stderr,
                     "vst2: rejecting '%s' — insane AEffect counts "
                     "(in=%d out=%d params=%d)\n",
                     desc.path.c_str(), effect_->numInputs,
                     effect_->numOutputs, effect_->numParams);
        release();
        return false;
    }

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

bool Vst2PluginInstance::validateEffectCounts() const
{
    if (!effect_) return false;
    return effect_->numInputs  >= 0 && effect_->numInputs  <= kMaxPluginChannels &&
           effect_->numOutputs >= 0 && effect_->numOutputs <= kMaxPluginChannels &&
           effect_->numParams  >= 0 && effect_->numParams  <= kMaxPluginParams;
}

// ---------------------------------------------------------------------------
// adoptEffectForTest — TEST-ONLY entry: wire up an in-process fake AEffect so
// vst2_test can exercise the hardening paths without a plugin DLL.
// ---------------------------------------------------------------------------
bool Vst2PluginInstance::adoptEffectForTest(AEffect* eff)
{
    if (!eff || eff->magic != kEffectMagic) return false;

    effect_ = eff;
    if (!validateEffectCounts())
    {
        effect_ = nullptr;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        registry()[effect_] = this;
    }
    dispatch(effOpen, 0, 0, nullptr, 0.0f);
    opened_ = true;
    if (dead_.load(std::memory_order_relaxed))
    {
        release();
        return false;
    }

    numIn_  = effect_->numInputs;
    numOut_ = effect_->numOutputs;
    desc_.format       = PluginFormat::VST2;
    desc_.name         = "test effect";
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
    if (!effect_ || maxBlockSize <= 0) return false;

    // A re-prepare must not resize buffers the audio thread may be inside;
    // gate out any in-flight block first (no-op on the first prepare).
    quiesceProcessing();
    prepared_ = false;

    sampleRate_   = sampleRate;
    maxBlockSize_ = maxBlockSize;

    dispatch(effSetSampleRate, 0, 0, nullptr, (float)sampleRate_);
    dispatch(effSetBlockSize, 0, (intptr_t)maxBlockSize_, nullptr, 0.0f);

    // Counts were load-validated, but the multiplied sizes can still exhaust
    // memory — fail the prepare gracefully rather than terminate on bad_alloc.
    try
    {
        allocChannelBuffers();
    }
    catch (...)
    {
        freeChannelBuffers();
        return false;
    }

    // Pre-allocate a MIDI event buffer big enough for a generous block.
    // (freeEventBuffer() resets maxEvents_ to 0, so set the count AFTER it.)
    freeEventBuffer();
    maxEvents_ = 1024;
    // VstEvents has a trailing events[1]; over-allocate for maxEvents_ ptrs.
    size_t evBytes = sizeof(VstEvents) +
                     (size_t)(maxEvents_ - 1) * sizeof(VstEvent*);
    vstEvents_ = (VstEvents*)std::malloc(evBytes);
    eventStorage_ = std::malloc((size_t)maxEvents_ * sizeof(VstMidiEvent));
    if (!vstEvents_ || !eventStorage_)
    {
        freeEventBuffer();
        freeChannelBuffers();
        return false;
    }
    std::memset(vstEvents_, 0, evBytes);
    std::memset(eventStorage_, 0, (size_t)maxEvents_ * sizeof(VstMidiEvent));

    prepared_ = true;

    // Turn the plugin on (resume), then open the gate for the audio thread.
    setActive(true);
    alive_.store(true, std::memory_order_seq_cst);
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
    inStorage_.assign((size_t)std::max(numIn_, 0) * (size_t)maxBlockSize_, 0.0f);

    // Writable discard blocks for plugin output channels the caller lacks
    // (a 16-out drum plugin against our stereo bus still needs 14 valid
    // buffers to write into). One distinct block per plugin output channel.
    dump_.assign((size_t)std::max(numOut_, 0) * (size_t)maxBlockSize_, 0.0f);
}

void Vst2PluginInstance::freeChannelBuffers()
{
    inPtrs_.clear();
    outPtrs_.clear();
    inStorage_.clear();
    silence_.clear();
    dump_.clear();
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
    // Teardown gate: no audio block may be inside the plugin (or its
    // buffers) while we close it and free them.
    quiesceProcessing();
    prepared_ = false;

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
// Allocation- and lock-free. Hardened three ways: every blk.audioIn/audioOut
// index is clamped to the CALLER's declared channel counts (a 16-out drum
// plugin against our stereo bus must never make us read past the caller's
// pointer arrays), every call into the plugin runs under the SEH fault
// guard, and the processing_/alive_ pair gates concurrent teardown (see
// quiesceProcessing).
// ---------------------------------------------------------------------------

// Zero the caller's DECLARED output channels — never index past numAudioOut.
static void zeroCallerOutputs(const ProcessBlock& blk, int n)
{
    if (!blk.audioOut) return;
    for (int c = 0; c < (int)blk.numAudioOut; ++c)
        if (blk.audioOut[c])
            std::memset(blk.audioOut[c], 0, (size_t)n * sizeof(float));
}

static void normalizeCallerOutputs(const ProcessBlock& blk, int n, int pluginOutputs)
{
    if (!blk.audioOut || n <= 0) return;
    const int callerOutputs = std::max(0, (int)blk.numAudioOut);
    if (pluginOutputs == 1 && callerOutputs > 1 && blk.audioOut[0]) {
        for (int c = 1; c < callerOutputs; ++c)
            if (blk.audioOut[c])
                std::memcpy(blk.audioOut[c], blk.audioOut[0], (size_t)n * sizeof(float));
    }
}

void Vst2PluginInstance::process(const ProcessBlock& blk)
{
    // Teardown gate: raise processing_ BEFORE reading alive_ (both seq_cst)
    // so either a concurrent quiesceProcessing() observes us in-flight and
    // waits, or we observe alive_ == false and bail before touching state.
    processing_.store(true, std::memory_order_seq_cst);
    if (!alive_.load(std::memory_order_seq_cst) || !effect_ || !prepared_)
    {
        if (blk.nframes > 0) zeroCallerOutputs(blk, blk.nframes);
        processing_.store(false, std::memory_order_release);
        return;
    }

    const int n = blk.nframes;
    if (n <= 0 || n > maxBlockSize_)
    {
        processing_.store(false, std::memory_order_release);
        return;
    }

    // A previously-faulted instance degrades to silence, permanently.
    if (dead_.load(std::memory_order_relaxed))
    {
        zeroCallerOutputs(blk, n);
        processing_.store(false, std::memory_order_release);
        return;
    }

    // Snapshot transport for audioMasterGetTime (read by plugin during process).
    curTempo_   = blk.tempoBpm > 0.0 ? blk.tempoBpm : 120.0;
    curPlayPos_ = blk.playPositionSamples;
    curPlaying_ = blk.isPlaying;

    // --- parameter automation (block-start, not sample-accurate for v1) ----
    // Index validated as UNSIGNED: a huge uint32 id must not cast negative
    // and slip past a signed '<' check into setParameter.
    for (int i = 0; i < blk.numParamIn; ++i)
    {
        const ParamChange& pc = blk.paramIn[i];
        if (pc.id < (uint32_t)effect_->numParams)
            guardedSetParameter((int32_t)pc.id, pc.value);
    }

    // --- MIDI delivery ------------------------------------------------------
    if (blk.numMidiIn > 0 && blk.midiIn && vstEvents_ && eventStorage_)
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
            // CLAMP out-of-range offsets to the block instead of zeroing:
            // zeroing fired next-block events EARLY (a whole block, ~10.7ms).
            // The holdback layer should already guarantee [0, n); this is the
            // last-resort fence for producer overshoot / shortened blocks.
            e.deltaFrames = std::min(std::max(m.sampleOffset, 0), n - 1);
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

    // A dispatcher/parameter call above may have faulted mid-block; do not
    // hand a plugin that just crashed another entry point.
    if (dead_.load(std::memory_order_relaxed))
    {
        zeroCallerOutputs(blk, n);
        processing_.store(false, std::memory_order_release);
        return;
    }

    // The caller's REAL channel counts — never index blk.audioIn/audioOut
    // past these, whatever the plugin's own numInputs/numOutputs claim.
    const int callerIn  = blk.audioIn  ? (int)blk.numAudioIn  : 0;
    const int callerOut = blk.audioOut ? (int)blk.numAudioOut : 0;

    zeroCallerOutputs(blk, n);

    // --- build input pointer array -----------------------------------------
    for (int c = 0; c < numIn_; ++c)
    {
        const float* src = (c < callerIn) ? blk.audioIn[c] : nullptr;
        if (src)
            inPtrs_[(size_t)c] = const_cast<float*>(src);
        else
        {
            // Use (and clear) our own scratch block as silent input.
            float* p = inStorage_.data() + (size_t)c * (size_t)maxBlockSize_;
            std::memset(p, 0, (size_t)n * sizeof(float));
            inPtrs_[(size_t)c] = p;
        }
    }

    // --- output pointer array (caller's buffers where declared, otherwise a
    // per-channel writable discard block so the plugin always gets valid
    // memory for every output it claims) --------------------------------------
    for (int c = 0; c < numOut_; ++c)
    {
        float* dst = (c < callerOut) ? blk.audioOut[c] : nullptr;
        outPtrs_[(size_t)c] =
            dst ? dst : dump_.data() + (size_t)c * (size_t)maxBlockSize_;
    }

    float** ins  = numIn_  > 0 ? inPtrs_.data()  : nullptr;
    float** outs = numOut_ > 0 ? outPtrs_.data() : nullptr;

    if (effect_->processReplacing || effect_->process)
    {
        const bool replacing = effect_->processReplacing != nullptr;
        ProcessCall call{ effect_, ins, outs, n, replacing };
        uint32_t code = 0;
        if (!seh_guarded_call(&runProcessCall, &call, &code))
        {
            // The plugin faulted mid-block: whatever it wrote is garbage.
            // Hand the caller silence and disable the instance for good.
            markDead(replacing ? "processReplacing" : "process", code);
            zeroCallerOutputs(blk, n);
        }
    }
    normalizeCallerOutputs(blk, n, numOut_);

    processing_.store(false, std::memory_order_release);
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
        info.defaultValue = guardedGetParameter(index);
    }
    return info;
}

// Both accessors validate the id as UNSIGNED (a huge uint32 must not cast
// negative past a signed check) and go through the fault-guarded raw calls.
float Vst2PluginInstance::getParamNormalized(uint32_t id) const
{
    if (effect_ && id < (uint32_t)effect_->numParams)
        return guardedGetParameter((int32_t)id);
    return 0.0f;
}

void Vst2PluginInstance::setParamNormalized(uint32_t id, float v)
{
    if (effect_ && id < (uint32_t)effect_->numParams)
        guardedSetParameter((int32_t)id, v);
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
        float v = guardedGetParameter(i);
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
        guardedSetParameter(i, v);
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

}} // namespace PatchKnob::engine
