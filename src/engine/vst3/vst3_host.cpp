//----------------------------------------------------------------------------
//  PatchKnob - VST3 host module implementation.
//
//  Implements PatchKnob::engine::IPluginInstance on Steinberg's VST3 SDK hosting
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
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <thread>
#include <chrono>

// Linux plug-in views need an IRunLoop from the host (there is no global event
// loop on X11), and it is delivered through the IPlugFrame we hand to
// IPlugView::setFrame().  <poll.h> drives the file-descriptor half.
#if !defined(_WIN32) && !defined(__APPLE__)
  #define PATCHKNOB_VST3_X11 1
  #include <poll.h>
#else
  #define PATCHKNOB_VST3_X11 0
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace PatchKnob { namespace engine {

namespace {

std::string u16ToUtf8(const Vst::TChar* s)
{
    return Vst::StringConvert::convert(s);
}

// Maps a raw 3-byte MIDI controller index from a CC status byte to the VST3
// CtrlNumber used by IMidiMapping. Channel-mode and standard CCs are 0..127.
inline CtrlNumber ccToCtrlNumber(uint8_t data1) { return static_cast<CtrlNumber>(data1); }

// The IPlugView platform type this host embeds into.  Hard-coding kPlatformTypeHWND
// made every VST3 editor unreachable on Linux (isPlatformTypeSupported returns
// kResultFalse, so hasEditor() said "no editor" and the user silently got the
// fallback parameter panel).  See pluginterfaces/gui/iplugview.h.
#if defined(_WIN32)
const FIDString kHostPlatformType = kPlatformTypeHWND;
#elif defined(__APPLE__)
const FIDString kHostPlatformType = kPlatformTypeNSView;
#else
const FIDString kHostPlatformType = kPlatformTypeX11EmbedWindowID;
#endif

// Per-block MIDI capacity of the engine's node ports (patch_graph.h
// kNodeMidiCap).  The VST3 EventList must be at least this big or addEvent()
// starts failing silently mid-block.
constexpr int32 kHostMidiCap = 512;

void clearCallerOutputs(const ProcessBlock& blk)
{
    if (!blk.audioOut || blk.nframes <= 0) return;
    for (int c = 0; c < std::max(0, (int)blk.numAudioOut); ++c)
        if (blk.audioOut[c])
            std::memset(blk.audioOut[c], 0, sizeof(float) * static_cast<size_t>(blk.nframes));
}

// Mono -> stereo upmix of the MAIN bus.
//
// This used to take the FLAT output total (descriptor.numAudioOut) as its cue,
// which is exactly the multi-bus bug: a plugin with a mono main bus and any
// second bus reports >= 2, the upmix was skipped, and caller channel 1 kept
// whatever the next bus had written there.  The cue is bus 0's channel count,
// and the copy stays inside bus 0's caller slot (see kPluginBusSlotChannels) so
// it can never overwrite a real channel of another bus.
//
// `mainOutChannels` == the flat total when the layout is unknown, which
// reproduces the old behaviour for a genuinely mono single-bus plugin.
void normalizeCallerOutputs(const ProcessBlock& blk, int mainOutChannels, int mainSlotWidth)
{
    if (!blk.audioOut || blk.nframes <= 0 || mainOutChannels != 1) return;
    const int callerOutputs = std::max(0, (int)blk.numAudioOut);
    if (callerOutputs < 2 || !blk.audioOut[0]) return;
    const int last = std::min(callerOutputs, std::max(2, mainSlotWidth));
    for (int c = 1; c < last; ++c)
        if (blk.audioOut[c])
            std::memcpy(blk.audioOut[c], blk.audioOut[0],
                        sizeof(float) * static_cast<size_t>(blk.nframes));
}

//============================================================================
//  Host-side IPlugFrame.  Two jobs:
//    * answer IPlugView::setFrame() at all -- a plug-in that never gets a frame
//      has nobody to ask for a resize, and on X11 nobody to ask for timers;
//    * on Linux, BE the Steinberg::Linux::IRunLoop.  X11 plug-ins register
//      timers and X-connection file descriptors with it and never repaint
//      otherwise.  We pump it from idleEditor() on the message thread.
//
//  Lifetime: one frame per plug-in instance, owned by Impl and always
//  outliving the view (teardown() drops the view first).  Reference counting is
//  therefore a no-op with a non-zero count, the usual pattern for a
//  host-owned static-lifetime callback object.
//============================================================================
class HostPlugFrame : public IPlugFrame
#if PATCHKNOB_VST3_X11
                    , public Linux::IRunLoop
#endif
{
public:
    HostPlugFrame() = default;
    virtual ~HostPlugFrame() = default;

    // --- FUnknown ---------------------------------------------------------
    tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override
    {
        if (!obj) return kInvalidArgument;
        if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid) ||
            FUnknownPrivate::iidEqual(_iid, IPlugFrame::iid))
        {
            *obj = static_cast<IPlugFrame*>(this);
            addRef();
            return kResultOk;
        }
#if PATCHKNOB_VST3_X11
        if (FUnknownPrivate::iidEqual(_iid, Linux::IRunLoop::iid))
        {
            *obj = static_cast<Linux::IRunLoop*>(this);
            addRef();
            return kResultOk;
        }
#endif
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef()  override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }

    // --- IPlugFrame -------------------------------------------------------
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override
    {
        if (!view || !newSize) return kInvalidArgument;
        // We do not own the container window (sdlui/plugin_editor_window.cpp
        // does, and it tracks the ui::Window frame), so all we can honestly do
        // is accept the new size and tell the view to lay itself out for it.
        // getEditorSize() then reports the updated rect to the UI.
        return view->onSize(newSize);
    }

#if PATCHKNOB_VST3_X11
    // --- Linux::IRunLoop --------------------------------------------------
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler,
                                            Linux::FileDescriptor fd) override
    {
        if (!handler) return kInvalidArgument;
        for (auto& e : fds_)
            if (e.handler == handler && e.fd == fd) return kResultOk;
        fds_.push_back(FdEntry{handler, fd});
        return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override
    {
        if (!handler) return kInvalidArgument;
        bool hit = false;
        for (auto& e : fds_)
            if (e.handler == handler) { e.handler = nullptr; hit = true; }
        if (!pumping_) compact();
        return hit ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler,
                                     Linux::TimerInterval ms) override
    {
        if (!handler) return kInvalidArgument;
        if (ms == 0) ms = 1;                 // a 0ms timer would spin
        timers_.push_back(TimerEntry{handler, ms, nowMs()});
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override
    {
        if (!handler) return kInvalidArgument;
        bool hit = false;
        for (auto& t : timers_)
            if (t.handler == handler) { t.handler = nullptr; hit = true; }
        if (!pumping_) compact();
        return hit ? kResultOk : kResultFalse;
    }
#endif

    // Drive one pass of the run loop.  Message thread only; called from
    // Vst3PluginInstance::idleEditor().  A no-op where there is no run loop.
    void pump()
    {
#if PATCHKNOB_VST3_X11
        if (pumping_) return;                // never re-enter from a handler
        pumping_ = true;

        // 1) Ready file descriptors (non-blocking: timeout 0).
        if (!fds_.empty())
        {
            pollfds_.clear();
            pollfds_.reserve(fds_.size());
            for (const auto& e : fds_)
                if (e.handler) pollfds_.push_back(pollfd{e.fd, POLLIN, 0});
            if (!pollfds_.empty() &&
                ::poll(pollfds_.data(), static_cast<nfds_t>(pollfds_.size()), 0) > 0)
            {
                for (const auto& pfd : pollfds_)
                {
                    if (!(pfd.revents & (POLLIN | POLLERR | POLLHUP))) continue;
                    // Re-look-up by fd: a handler may have unregistered another
                    // handler during an earlier callback in this same pass.
                    for (size_t i = 0; i < fds_.size(); ++i)
                        if (fds_[i].handler && fds_[i].fd == pfd.fd)
                            fds_[i].handler->onFDIsSet(pfd.fd);
                }
            }
        }

        // 2) Due timers.  Index-based: a handler may append to timers_.
        const uint64_t now = nowMs();
        for (size_t i = 0; i < timers_.size(); ++i)
        {
            TimerEntry& t = timers_[i];
            if (!t.handler || now - t.last < t.intervalMs) continue;
            t.last = now;
            t.handler->onTimer();
        }

        pumping_ = false;
        compact();
#endif
    }

private:
#if PATCHKNOB_VST3_X11
    struct FdEntry    { Linux::IEventHandler* handler; Linux::FileDescriptor fd; };
    struct TimerEntry { Linux::ITimerHandler* handler; uint64_t intervalMs; uint64_t last; };

    static uint64_t nowMs()
    {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }
    void compact()
    {
        fds_.erase(std::remove_if(fds_.begin(), fds_.end(),
                                  [](const FdEntry& e) { return e.handler == nullptr; }),
                   fds_.end());
        timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                     [](const TimerEntry& t) { return t.handler == nullptr; }),
                      timers_.end());
    }

    std::vector<FdEntry>    fds_;
    std::vector<TimerEntry> timers_;
    std::vector<pollfd>     pollfds_;
    bool                    pumping_ = false;
#endif
};

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

    //------------------------------------------------------------------------
    //  Host-side IComponentHandler -- the bridge this class's banner comment
    //  always promised but nothing implemented.  A VST3 editor never touches
    //  the processor: a knob move goes to the plug-in's own IEditController,
    //  which reports it to the HOST through this interface (beginEdit /
    //  performEdit / endEdit).  No handler was ever installed, so every edit
    //  from a plug-in's native GUI was dropped on the floor: the knob animated
    //  (the controller's value moved, read-back looked right) while the DSP
    //  never heard a thing.  performEdit now parks the edit in the SAME
    //  pending-parameter ring the fallback panel path uses, and process()
    //  turns it into an IParameterChanges point at the top of the next block.
    //
    //  Threading: the editor lives on the message thread, so performEdit and
    //  restartComponent arrive there; the ring is the wait-free message->audio
    //  SPSC path built for exactly this.  No allocation, no locks.
    //  Lifetime: member of Impl, installed on the controller in load() and
    //  uninstalled in teardown() BEFORE the controller is released, so it
    //  always outlives the plug-in's use of it.  Static-lifetime refcount
    //  pattern, same as HostPlugFrame.
    //------------------------------------------------------------------------
    struct HostComponentHandler : public IComponentHandler {
        Impl& o;
        explicit HostComponentHandler(Impl& owner) : o(owner) {}
        virtual ~HostComponentHandler() = default;

        tresult PLUGIN_API queryInterface(const TUID _iid, void** obj) override
        {
            if (!obj) return kInvalidArgument;
            if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid) ||
                FUnknownPrivate::iidEqual(_iid, IComponentHandler::iid))
            {
                *obj = static_cast<IComponentHandler*>(this);
                addRef();
                return kResultOk;
            }
            *obj = nullptr;
            return kNoInterface;
        }
        uint32 PLUGIN_API addRef()  override { return 1000; }
        uint32 PLUGIN_API release() override { return 1000; }

        tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
        tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) override
        {
            float v = static_cast<float>(valueNormalized);
            if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
            auto it = o.paramIndexById.find(static_cast<uint32_t>(id));
            if (it == o.paramIndexById.end() || it->second >= o.pending.size())
                return kResultFalse;
            o.pushPendingParam(it->second, v);
            return kResultOk;
        }
        tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
        tresult PLUGIN_API restartComponent(int32 flags) override
        {
            // A preset/program load inside the plug-in's own UI announces
            // itself here: re-read EVERY parameter from the controller and
            // push it to the DSP, or the preset would be exactly as inaudible
            // as the knobs were.  The ring holds one slot per parameter by
            // construction (cacheParams), so a full push cannot overflow it.
            if ((flags & kParamValuesChanged) && o.controller)
                for (size_t i = 0; i < o.params.size(); ++i)
                    o.pushPendingParam(static_cast<uint32_t>(i),
                        static_cast<float>(o.controller->getParamNormalized(
                            static_cast<ParamID>(o.params[i].id))));
            return kResultOk;
        }
    };
    HostComponentHandler compHandler{*this};

    // --- host context -------------------------------------------------------
    // PluginContextFactory holds ONE global context pointer for the whole
    // process.  It used to be set to a per-instance member: loading plug-in B
    // repointed the global at B's object, and releasing B destroyed it while
    // plug-in A -- still loaded, still holding the pointer it was handed in
    // initialize() -- kept using it.  One process-wide instance, never
    // destroyed, is the only shape that matches the factory's lifetime.
    // Deliberately never destroyed: a plug-in module that is still resident at
    // process exit may call back into its host context, and a function-local
    // static would already have run its destructor by then.
    static HostApplication& globalHostApp()
    {
        static HostApplication* app = new HostApplication();
        return *app;
    }

    // --- prepared state -----------------------------------------------------
    double sampleRate   = 0.0;
    int    maxBlockSize = 0;
    bool   prepared     = false;
    // Read by process() on the audio thread and written by setActive() on the
    // message thread, so it is atomic rather than a plain bool.
    std::atomic<bool> active{false};

    int32 numAudioInBuses  = 0;
    int32 numAudioOutBuses = 0;
    int   eventInBusIndex  = -1;   // first event input bus (MIDI), or -1

    // Plain-scalar mirror of the descriptor's bus layout, published by
    // refreshBusLayout() on the message thread and read by process() on the
    // audio thread.  process() must NOT walk descriptor.audio*Buses directly:
    // those are std::vectors that a re-prepare rebuilds, and an audio thread has
    // no business reading a container someone else may be reallocating (the
    // realtime gate already serialises the two, but keeping RT reads to plain
    // ints costs nothing and removes the question entirely).
    int rtCallerInChannels  = 0;   // caller channels this layout can reach (slots)
    int rtCallerOutChannels = 0;
    int rtMainOutChannels   = 0;   // bus 0's channel count -- the upmix cue
    int rtMainOutSlotWidth  = 0;

    // --- pre-allocated realtime structures (reused every block) -------------
    HostProcessData processData;
    // MUST be >= the ProcessBlock MIDI port capacity (kMaxMidiEvents in
    // plugin_api.h).  It was 256 against a port of 512: EventList::addEvent()
    // fails SILENTLY once full, and since note-offs trail their note-ons in the
    // block, a dense chord or a CC sweep dropped exactly the note-offs -> hung
    // notes.  Sized from the port constant so the two can never drift again.
    EventList       inputEvents{kHostMidiCap};
    ParameterChanges inputParamChanges{64};
    ParameterChanges outputParamChanges{64};
    ProcessContext  processContext{};

    // --- realtime <-> message thread gate -----------------------------------
    // process() runs on the audio thread; loadState() reallocates the plug-in's
    // internal state on the message thread.  Same shape as the VST2 host's
    // quiesceProcessing(): process() raises `processing` FIRST and re-reads
    // `gateOpen`, so either the message thread sees us in flight and waits, or
    // we see the closed gate and bail before entering the plug-in.
    std::atomic<bool> gateOpen{true};
    std::atomic<bool> processing{false};

    void quiesceProcessing();
    void resumeProcessing() { gateOpen.store(true, std::memory_order_seq_cst); }

    // --- audio -> UI parameter feedback -------------------------------------
    // IEditController is a MESSAGE-THREAD interface; process() used to call
    // setParamNormalized() on it directly every block.  Output parameter
    // changes now go through this wait-free single-producer/single-consumer
    // ring and are applied by drainParamFeedback() from the UI thread.
    struct ParamFeedback { uint32_t id; float value; };
    static constexpr uint32_t kFeedbackSlots = 512;   // power of two
    ParamFeedback         feedback[kFeedbackSlots];
    std::atomic<uint32_t> feedbackWrite{0};
    std::atomic<uint32_t> feedbackRead{0};

    void pushParamFeedback(uint32_t id, float v)      // audio thread
    {
        const uint32_t w = feedbackWrite.load(std::memory_order_relaxed);
        const uint32_t r = feedbackRead.load(std::memory_order_acquire);
        if (w - r >= kFeedbackSlots) return;          // full: drop (UI cache only)
        feedback[w & (kFeedbackSlots - 1)] = ParamFeedback{id, v};
        feedbackWrite.store(w + 1, std::memory_order_release);
    }
    void drainParamFeedback()                          // message thread
    {
        uint32_t r = feedbackRead.load(std::memory_order_relaxed);
        const uint32_t w = feedbackWrite.load(std::memory_order_acquire);
        for (; r != w; ++r)
        {
            const ParamFeedback& f = feedback[r & (kFeedbackSlots - 1)];
            if (controller)
                controller->setParamNormalized(static_cast<ParamID>(f.id), f.value);
        }
        feedbackRead.store(r, std::memory_order_release);
    }

    // --- UI -> audio parameter edits ----------------------------------------
    // The mirror image of the feedback ring above, and the thing that used to be
    // missing entirely.  In VST3 the IEditController and the IAudioProcessor are
    // SEPARATE components: IEditController::setParamNormalized() moves only the
    // controller's (GUI's) copy of the value.  The DSP half hears nothing until
    // the change is delivered as an IParameterChanges point inside ProcessData.
    // Edits made from our own fallback parameter panel are therefore parked here
    // on the message thread and drained into inputParamChanges at the top of
    // process() on the audio thread.
    //
    // Shape: one slot per cached parameter (sized in cacheParams()) plus a
    // wait-free SPSC ring of parameter INDICES.  A parameter is pushed onto the
    // ring only on the 0->1 edge of its dirty flag -- a repeat edit of the same
    // parameter overwrites the parked value in place -- so at most one entry per
    // parameter is ever in flight and a ring sized to the parameter count can
    // never overflow.  Nothing is ever dropped and the newest value always wins,
    // which is what keeps a fast slider drag from diverging from the DSP.
    //
    // The dirty/value pair is seq_cst on purpose: the consumer clears `dirty`
    // BEFORE reading `value`, so an edit landing between the two either sees the
    // flag still set (and its store is ordered ahead of the consumer's read) or
    // re-queues itself for the next block.  Neither path loses the edit.
    struct PendingParam {
        std::atomic<float> value{0.0f};
        std::atomic<bool>  dirty{false};
    };
    std::vector<PendingParam> pending;        // parallel to `params`
    std::vector<uint32_t>     pendingRing;    // holds indices into `params`
    uint32_t                  pendingMask = 0;
    std::atomic<uint32_t>     pendingWrite{0};
    std::atomic<uint32_t>     pendingRead{0};
    std::unordered_map<uint32_t, uint32_t> paramIndexById;   // ParamID -> index

    void pushPendingParam(uint32_t index, float v)   // message thread
    {
        pending[index].value.store(v);
        if (pending[index].dirty.exchange(true))
            return;                                  // already queued; value refreshed
        const uint32_t w = pendingWrite.load(std::memory_order_relaxed);
        pendingRing[w & pendingMask] = index;
        pendingWrite.store(w + 1, std::memory_order_release);
    }

    // Scratch channel-pointer arrays so process() never allocates. These point
    // at the caller's planar buffers (and at a shared silence buffer for any
    // channels the caller did not supply).
    std::vector<float*> inChanPtrs;
    std::vector<float*> outChanPtrs;
    std::vector<float>  silence;     // zero input for unconnected channels
    std::vector<float>  dump;        // discard sink for extra output channels

    // --- editor -------------------------------------------------------------
    // `frame` is handed to the view via setFrame() BEFORE attached().  On X11 it
    // is also the plug-in's IRunLoop; without it X11 editors get no timers and
    // never repaint.  Declared BEFORE `view` so it outlives it (members are
    // destroyed in reverse declaration order).
    HostPlugFrame   frame;
    IPtr<IPlugView> view;
    bool            editorOpen = false;

    // --- parameter cache ----------------------------------------------------
    std::vector<ParamInfo> params;

    Impl() = default;
    ~Impl() { teardown(); }

    void cacheParams();
    bool setupProcessingChain();
    // Re-read the bus topology from the component and republish it on the
    // descriptor.  MUST run after setBusArrangements(): a plugin may answer a
    // negotiation with a different channel count than its default BusInfo, and
    // the descriptor has to advertise what the plugin actually agreed to (the
    // counts used to be frozen at load() from DEFAULT bus info and never
    // refreshed, so they could over-report for the entire life of the instance).
    void refreshBusLayout();
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

    // Rebuild the UI->audio edit parking lot to match the new parameter list.
    // Message thread, before any process() can run against this instance (both
    // load() and the reload paths quiesce first), so plain assignment is safe.
    paramIndexById.clear();
    for (size_t i = 0; i < params.size(); ++i)
        paramIndexById.emplace(params[i].id, static_cast<uint32_t>(i));

    pending = std::vector<PendingParam>(params.size());
    uint32_t cap = 1;
    while (cap < params.size()) cap <<= 1;      // >= paramCount, power of two
    pendingRing.assign(cap, 0u);
    pendingMask = cap - 1;
    pendingWrite.store(0, std::memory_order_relaxed);
    pendingRead.store(0, std::memory_order_relaxed);
}

// Snapshot the component's audio bus topology onto the descriptor.
//
// Sources, in the order the VST3 SDK intends them (ivstcomponent.h / IComponent
// ::getBusInfo, ivstaudioprocessor.h / IAudioProcessor::getBusArrangement):
//   * getBusInfo  -> name, busType (kMain / kAux) and the channel count that
//                    HostProcessData::createBuffers also uses to size the
//                    AudioBusBuffers we hand the plugin (processdata.cpp), so it
//                    is the count that matches the buffers in flight;
//   * getBusArrangement -> what the processor says it settled on after
//                    setBusArrangements.  Where the two disagree we take the
//                    SMALLER, because over-reporting would advertise channels
//                    the buffers do not carry.
void Vst3PluginInstance::Impl::refreshBusLayout()
{
    descriptor.audioInBuses.clear();
    descriptor.audioOutBuses.clear();
    descriptor.numAudioIn  = 0;
    descriptor.numAudioOut = 0;
    if (!component)
        return;

    auto collect = [&](BusDirection dir, std::vector<PluginBusInfo>& outBuses, int& total) {
        const int32 n = component->getBusCount(kAudio, dir);
        outBuses.reserve(static_cast<size_t>(std::max<int32>(n, 0)));
        for (int32 i = 0; i < n; ++i)
        {
            BusInfo bi = {};
            if (component->getBusInfo(kAudio, dir, i, bi) != kResultOk)
                continue;
            int channels = static_cast<int>(bi.channelCount);
            if (processor)
            {
                SpeakerArrangement arr = 0;
                if (processor->getBusArrangement(dir, i, arr) == kResultOk)
                {
                    const int negotiated =
                        static_cast<int>(SpeakerArr::getChannelCount(arr));
                    if (negotiated >= 0 && negotiated < channels)
                        channels = negotiated;
                }
            }
            if (channels < 0) channels = 0;

            PluginBusInfo pb;
            pb.name         = u16ToUtf8(bi.name);
            pb.channelCount = channels;
            pb.isMain       = (bi.busType == kMain);
            pb.isAux        = (bi.busType == kAux);
            if (pb.name.empty())
                pb.name = outBuses.empty() ? "Main" : ("Bus " + std::to_string(i + 1));
            outBuses.push_back(std::move(pb));
            total += channels;
        }
    };

    collect(kInput,  descriptor.audioInBuses,  descriptor.numAudioIn);
    collect(kOutput, descriptor.audioOutBuses, descriptor.numAudioOut);

    rtCallerInChannels  = pluginBusCallerChannels(descriptor.audioInBuses,
                                                  descriptor.numAudioIn);
    rtCallerOutChannels = pluginBusCallerChannels(descriptor.audioOutBuses,
                                                  descriptor.numAudioOut);
    rtMainOutChannels   = descriptor.mainOutChannels();
    rtMainOutSlotWidth  = descriptor.audioOutBuses.empty()
                            ? 0                     // unknown: upmix fills all
                            : pluginBusSlotWidth(rtMainOutChannels);
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

    // The input queue must be able to hold a point for EVERY parameter in one
    // block: a full drain of the pending-edit ring (below) can touch all of them
    // at once, and addParameterData() returns null past its cap -- which would
    // silently swallow an edit whose dirty flag has already been cleared.
    inputParamChanges.setMaxParameters(
        std::max<int32>(64, static_cast<int32>(params.size())));
    outputParamChanges.setMaxParameters(
        std::max<int32>(64, static_cast<int32>(params.size())));

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

    // 5) The negotiation above may have changed the buses; republish them so
    //    the descriptor advertises what the plugin AGREED to, not its defaults.
    refreshBusLayout();

    return true;
}

// Message-thread half of the realtime gate: close the gate, then spin until any
// in-flight process() block has left the plug-in.  An audio block is a couple of
// milliseconds at most, so a yield loop is the right primitive here (the same
// one the VST2 host uses).
void Vst3PluginInstance::Impl::quiesceProcessing()
{
    gateOpen.store(false, std::memory_order_seq_cst);
    while (processing.load(std::memory_order_acquire))
        std::this_thread::yield();
}

void Vst3PluginInstance::Impl::teardown()
{
    quiesceProcessing();

    if (view)
    {
        if (editorOpen)
            view->removed();
        view->setFrame(nullptr);   // drop the frame BEFORE the view dies
        view = nullptr;
        editorOpen = false;
    }
    const bool wasActive = active.load(std::memory_order_relaxed);
    if (processor && wasActive)
        processor->setProcessing(false);
    if (component && wasActive)
        component->setActive(false);
    active.store(false, std::memory_order_relaxed);
    prepared = false;

    processData.unprepare();

    midiMapping = nullptr;
    processor   = nullptr;
    // Unhook our handler while the controller is still alive: a controller
    // that outlives its host handler (terminate() may raise late edits) must
    // not be left holding a pointer into this dying Impl.
    if (controller)
        controller->setComponentHandler(nullptr);
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
        static_cast<FUnknown*>(static_cast<IHostApplication*>(&Impl::globalHostApp())));

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

    // Bus topology from the plugin's DEFAULT arrangements.  prepare() re-runs
    // this through setupProcessingChain() once the arrangements are negotiated,
    // so the descriptor never stays stuck on defaults that the plugin later
    // contradicted.
    d_->refreshBusLayout();
    d_->descriptor.format      = PluginFormat::VST3;
    d_->descriptor.isInstrument = d_->component->getBusCount(kEvent, kInput) > 0;
    if (d_->descriptor.path.empty())
        d_->descriptor.path = desc.path;

    d_->cacheParams();

    // Wire the editor's return path LAST (after cacheParams built the ring the
    // handler pushes into) but before load() returns -- no editor can exist
    // until then.  Without this call the handler above is dead code and the
    // plug-in GUI's parameter edits silently vanish (the Dragonfly "knobs do
    // nothing" bug).
    if (d_->controller)
        d_->controller->setComponentHandler(&d_->compHandler);

    return true;
}

const PluginDescriptor& Vst3PluginInstance::descriptor() const { return d_->descriptor; }

bool Vst3PluginInstance::prepare(double sampleRate, int maxBlockSize)
{
    if (!d_->component || !d_->processor)
        return false;

    // Idempotent: if already set up for this exact sample rate / block size,
    // do nothing.  Re-running setupProcessing()/setBusArrangements() is not only
    // wasteful but some plugins reject the second negotiation and fail -- which
    // would leave prepared_ false and make a later setActive() a silent no-op.
    // (This also preserves the active state across a redundant prepare, e.g. the
    // one PatchGraph::addNode issues after the caller already prepared.)
    if (d_->prepared && d_->sampleRate == sampleRate && d_->maxBlockSize == maxBlockSize)
        return true;

    // Re-preparing (changed rate/block) requires a clean (inactive) state first,
    // AND that no audio block is in flight: setupProcessingChain() re-runs
    // HostProcessData::prepare(), which frees and reallocates the AudioBusBuffers
    // arrays process() writes channel pointers into.
    d_->quiesceProcessing();
    struct Reopen {
        Impl* d;
        ~Reopen() { d->resumeProcessing(); }
    } reopen{ d_.get() };

    if (d_->active)
        setActiveLocked(false);

    d_->sampleRate   = sampleRate;
    d_->maxBlockSize = maxBlockSize;

    if (!d_->setupProcessingChain())
        return false;

    d_->prepared = true;
    return true;
}

// Caller must already hold the realtime gate closed (see quiesceProcessing).
void Vst3PluginInstance::setActiveLocked(bool active)
{
    if (!d_->component || !d_->processor || !d_->prepared)
        return;
    if (active == d_->active.load(std::memory_order_relaxed))
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
    d_->active.store(active, std::memory_order_seq_cst);
}

void Vst3PluginInstance::setActive(bool active)
{
    // IComponent::setActive()/IAudioProcessor::setProcessing() reconfigure the
    // plug-in's DSP state; they must not run next to a live process() call.
    d_->quiesceProcessing();
    struct Reopen {
        Impl* d;
        ~Reopen() { d->resumeProcessing(); }
    } reopen{ d_.get() };
    setActiveLocked(active);
}

void Vst3PluginInstance::release() { d_->teardown(); }

void Vst3PluginInstance::process(const ProcessBlock& blk)
{
    Impl& s = *d_;
    // Realtime gate (see Impl::quiesceProcessing): raise `processing` BEFORE
    // reading `gateOpen`, both seq_cst, so a concurrent loadState()/release()
    // either waits for us or we bail before entering the plug-in.
    s.processing.store(true, std::memory_order_seq_cst);
    if (!s.gateOpen.load(std::memory_order_seq_cst) ||
        !s.active.load(std::memory_order_seq_cst) || !s.processor)
    {
        clearCallerOutputs(blk);
        s.processing.store(false, std::memory_order_release);
        return;
    }

    const int32 nframes = blk.nframes;
    if (nframes <= 0)
    {
        s.processing.store(false, std::memory_order_release);
        return;
    }

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
            // CLAMP into [0, nframes-1].  The VST2 host has always fenced this;
            // VST3 never did, and an offset past the block makes plug-ins either
            // drop the event or index their own scratch out of bounds.  Clamping
            // (rather than zeroing) keeps a late event late instead of firing it
            // a whole block early.
            e.sampleOffset = std::min(std::max(m.sampleOffset, 0), nframes - 1);
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
            addParamPoint(static_cast<ParamID>(pc.id),
                          std::min(std::max(pc.sampleOffset, 0), nframes - 1),
                          static_cast<double>(pc.value));
        }
    }

    // Host-GUI edits (our fallback parameter panel).  Drained AFTER track
    // automation so a live slider drag wins the block: addPoint() REPLACES an
    // existing point at the same sample offset, so the last writer at offset 0
    // is the value the plug-in sees.  Offset 0 (block start) is accurate enough
    // for a hand-driven control at our block sizes.
    //
    // Wait-free and allocation-free: fixed-size ring, fixed-size slot table, and
    // the IParamValueQueue point vectors reach their steady-state capacity after
    // the first few blocks (clearQueue() keeps capacity).
    if (s.pendingMask != 0 || !s.pending.empty())
    {
        uint32_t r = s.pendingRead.load(std::memory_order_relaxed);
        const uint32_t w = s.pendingWrite.load(std::memory_order_acquire);
        for (; r != w; ++r)
        {
            const uint32_t idx = s.pendingRing[r & s.pendingMask];
            if (idx >= s.pending.size()) continue;
            // Clear FIRST, then read: an edit that lands in between re-queues
            // itself rather than being lost.  See the ring's declaration.
            s.pending[idx].dirty.store(false);
            addParamPoint(static_cast<ParamID>(s.params[idx].id), 0,
                          static_cast<double>(s.pending[idx].value.load()));
        }
        s.pendingRead.store(r, std::memory_order_release);
    }

    // Map raw CC / pitchbend to parameter changes via IMidiMapping if present.
    if (s.midiMapping && s.eventInBusIndex >= 0 && blk.midiIn)
    {
        for (int32 i = 0; i < blk.numMidiIn; ++i)
        {
            const MidiEvent& m = blk.midiIn[i];
            const uint8_t statusHi = m.status & 0xF0u;
            const int16   channel  = static_cast<int16>(m.status & 0x0Fu);
            // Same fence as the event path: an automation point past the block
            // is out of contract for IParamValueQueue::addPoint().
            const int32   offset   = std::min(std::max(m.sampleOffset, 0), nframes - 1);
            ParamID pid = 0;

            if (statusHi == 0xB0) // control change
            {
                if (s.midiMapping->getMidiControllerAssignment(
                        s.eventInBusIndex, channel, ccToCtrlNumber(m.data1), pid) == kResultOk)
                    addParamPoint(pid, offset, static_cast<double>(m.data2) / 127.0);
            }
            else if (statusHi == 0xE0) // pitch bend
            {
                if (s.midiMapping->getMidiControllerAssignment(
                        s.eventInBusIndex, channel, kPitchBend, pid) == kResultOk)
                {
                    const int bend = (static_cast<int>(m.data2) << 7) | static_cast<int>(m.data1);
                    addParamPoint(pid, offset, static_cast<double>(bend) / 16383.0);
                }
            }
            else if (statusHi == 0xD0) // channel pressure
            {
                if (s.midiMapping->getMidiControllerAssignment(
                        s.eventInBusIndex, channel, kAfterTouch, pid) == kResultOk)
                    addParamPoint(pid, offset, static_cast<double>(m.data1) / 127.0);
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

    // Clamp to the CALLER's declared channel counts as well as the plugin's:
    // never index blk.audioIn/audioOut past what the caller actually supplied
    // (a multi-out instrument can claim more channels than our stereo bus).
    // Clamp to the CALLER's declared channel counts (never index blk.audioIn/
    // audioOut past what the caller supplied) AND to how many caller channels
    // this plugin's bus layout can reach -- which is the SLOT total, not the
    // flat channel total, now that a mono bus occupies a stereo slot.
    const int callerIn  = std::min<int>(s.rtCallerInChannels,  blk.numAudioIn);
    const int callerOut = std::min<int>(s.rtCallerOutChannels, blk.numAudioOut);
    clearCallerOutputs(blk);

    // BUS-AWARE channel mapping.  Each plugin bus gets its own SLOT in the
    // caller's flat channel array (plugin_api.h kPluginBusSlotChannels): bus b
    // starts at caller channel `slot`, which advances by the bus's slot width
    // rather than by its raw channel count.  A stereo main bus therefore maps
    // 0,1 exactly as the old flat cursor did, but a MONO bus still consumes two
    // caller channels, which is what keeps every later bus lined up with the
    // caller's stereo port for that bus instead of sliding one channel left.
    //
    // The slot walk uses the LIVE AudioBusBuffers (the buffers actually handed
    // to the plugin), so it cannot disagree with what the plugin will write.
    // No allocation, no branch on descriptor state: RT-safe.

    // Inputs: point each channel of each input bus at the caller's buffer when
    // available, else at the shared silence buffer.
    int inSlot = 0;
    for (int32 b = 0; b < s.processData.numInputs; ++b)
    {
        AudioBusBuffers& bus = s.processData.inputs[b];
        bus.silenceFlags = 0;
        for (int32 c = 0; c < bus.numChannels; ++c)
        {
            const int callerCh = inSlot + static_cast<int>(c);
            float* p = nullptr;
            if (blk.audioIn && callerCh < callerIn)
                p = const_cast<float*>(blk.audioIn[callerCh]);
            if (!p)
            {
                p = s.silence.data();
                bus.silenceFlags |= (uint64(1) << c);
            }
            bus.channelBuffers32[c] = p;
        }
        inSlot += pluginBusSlotWidth(static_cast<int>(bus.numChannels));
    }

    // Outputs: same slotting; any channel that falls outside what the caller
    // supplied goes to the discard buffer so the plugin always has valid
    // storage for every channel it declares.
    int outSlot = 0;
    for (int32 b = 0; b < s.processData.numOutputs; ++b)
    {
        AudioBusBuffers& bus = s.processData.outputs[b];
        bus.silenceFlags = 0;
        for (int32 c = 0; c < bus.numChannels; ++c)
        {
            const int callerCh = outSlot + static_cast<int>(c);
            float* p = nullptr;
            if (blk.audioOut && callerCh < callerOut)
                p = blk.audioOut[callerCh];
            if (!p)
                p = s.dump.data();
            bus.channelBuffers32[c] = p;
        }
        outSlot += pluginBusSlotWidth(static_cast<int>(bus.numChannels));
    }

    // --- 5. Process ---------------------------------------------------------
    s.processData.inputEvents          = &s.inputEvents;
    s.processData.inputParameterChanges  = &s.inputParamChanges;
    s.processData.outputParameterChanges = &s.outputParamChanges;
    s.processData.processContext       = &s.processContext;

    s.processor->process(s.processData);
    normalizeCallerOutputs(blk, s.rtMainOutChannels,
                           s.rtMainOutSlotWidth > 0 ? s.rtMainOutSlotWidth
                                                    : std::max(0, (int)blk.numAudioOut));

    // --- 6. Hand output parameter changes to the MESSAGE thread -------------
    // IEditController is a message-thread interface; calling setParamNormalized()
    // from here (as this used to, every block) is a threading violation that
    // races the UI and, in plug-ins whose controller allocates or repaints on a
    // parameter change, allocates on the audio thread.  Queue instead; the UI
    // applies them in drainParamFeedback().
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
                s.pushParamFeedback(static_cast<uint32_t>(q->getParameterId()),
                                    static_cast<float>(val));
        }
    }

    s.processing.store(false, std::memory_order_release);
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
    // Apply anything process() queued since the last poll, so the fallback
    // parameter panel still tracks plug-in-driven moves when no native editor
    // (and therefore no idleEditor() pump) is open.  Message thread only.
    d_->drainParamFeedback();
    return static_cast<float>(d_->controller->getParamNormalized(static_cast<ParamID>(id)));
}

void Vst3PluginInstance::setParamNormalized(uint32_t id, float v)
{
    if (!d_->controller)
        return;
    if (v < 0.0f) v = 0.0f;
    else if (v > 1.0f) v = 1.0f;

    // 1) Controller half -- the plug-in's own GUI and our getParamNormalized()
    //    read-back both come from here, so this is what stops the slider from
    //    snapping back to its old position on the next repaint.
    d_->controller->setParamNormalized(static_cast<ParamID>(id), v);

    // 2) Processor half.  Step 1 alone changes NOTHING about the sound: the
    //    IAudioProcessor is a separate component and only reads parameter values
    //    out of ProcessData::inputParameterChanges.  Park the edit for the audio
    //    thread, which turns it into an automation point at the top of the next
    //    process() block.  Without this the panel was purely cosmetic.
    auto it = d_->paramIndexById.find(id);
    if (it != d_->paramIndexById.end() && it->second < d_->pending.size())
        d_->pushPendingParam(it->second, v);
}

bool Vst3PluginInstance::hasEditor() const
{
    if (!d_->controller)
        return false;
    if (!d_->view)
    {
        // Cache the view: hasEditor() is polled by the UI, and creating +
        // destroying an IPlugView on every poll is both wasteful and something
        // some plug-ins do not survive.  openEditor() reuses this one.
        d_->view = owned(d_->controller->createView(ViewType::kEditor));
        if (!d_->view)
            return false;
    }
    // kHostPlatformType, NOT a hard-coded kPlatformTypeHWND: on X11 the plug-in
    // only answers to kPlatformTypeX11EmbedWindowID, so the old constant made
    // every VST3 editor invisible to the Linux build.
    return d_->view->isPlatformTypeSupported(kHostPlatformType) == kResultTrue;
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
    if (d_->view->isPlatformTypeSupported(kHostPlatformType) != kResultTrue)
        return false;
    // The frame MUST be in place before attached(): X11 plug-ins ask the frame
    // for the IRunLoop from inside attached() and refuse to open without one.
    d_->view->setFrame(&d_->frame);
    if (d_->view->attached(parent, kHostPlatformType) != kResultOk)
    {
        d_->view->setFrame(nullptr);
        return false;
    }
    d_->editorOpen = true;
    return true;
}

void Vst3PluginInstance::closeEditor()
{
    if (d_->view && d_->editorOpen)
    {
        d_->view->removed();
        d_->view->setFrame(nullptr);
    }
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
    // On Windows/macOS the editor is driven by the native message loop and
    // there is nothing to do here.  On X11 there is no global event loop: WE
    // are the plug-in's IRunLoop, so this is where its registered timers and
    // file descriptors actually get serviced.
    d_->frame.pump();
    // Also the message-thread end of the audio->UI parameter feed (process()
    // must not touch IEditController itself).
    d_->drainParamFeedback();
}

std::vector<uint8_t> Vst3PluginInstance::saveState() const
{
    std::vector<uint8_t> out;
    if (!d_->component)
        return out;

    // Same gate as loadState(): IComponent::getState() walks the very state
    // process() is mutating.
    d_->quiesceProcessing();
    struct Reopen {
        Impl* d;
        ~Reopen() { d->resumeProcessing(); }
    } reopen{ d_.get() };

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

    // setState() makes the plug-in rebuild its internal state (buffers, voice
    // pools, coefficient tables).  Running that into a plug-in that the audio
    // thread is concurrently inside process() for is a use-after-free waiting
    // to happen -- and loading a project while the transport rolls does exactly
    // that.  Gate the audio thread out for the duration, exactly as the VST2
    // host does around prepare()/release().
    d_->quiesceProcessing();
    struct Reopen {
        Impl* d;
        ~Reopen() { d->resumeProcessing(); }
    } reopen{ d_.get() };

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
        // Say WHY the node stayed empty: a missing bundle, a wrong-architecture
        // module, a class id that no longer exists -- silent nullptrs here made
        // "add instrument" look like a no-op in the UI.
        std::fprintf(stderr, "[vst3] failed to load \"%s\" (uid \"%s\": missing "
                             "bundle, wrong architecture, or no such class)\n",
                     desc.path.c_str(), desc.uid.c_str());
        delete inst;
        return nullptr;
    }
    return inst;
}

}} // namespace PatchKnob::engine
