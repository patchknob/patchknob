//----------------------------------------------------------------------------
//  PatchKnob — explicit MIDI route table (header-only reference implementation).
//
//  This is the single source of truth the modular MIDI layer described in
//  DESIGN.md is built on.  Today PatchKnob decides MIDI routing by an IMPLICIT
//  convention — "bus index == track index == MIDI plug index" (src/audio_app.h,
//  the header comment on the track model) — and keeps two route tables that
//  nothing honours end-to-end (VirtualMidiPortsNode::routes_ in
//  src/engine/patch/patch_nodes.h:661, MidiTrackNode::input_/output_ at :721).
//  A convention cannot express "this keyboard drives those three instruments",
//  cannot be serialised, and cannot be shown in a UI.  A table can.
//
//  PROPERTIES (all of them load-bearing; see DESIGN.md §4):
//
//    * HEADER-ONLY.  No .cpp, no new library, no build-system churn — drop the
//      include in and it works.  That is deliberate: it lets the migration land
//      one call site at a time.
//    * ALLOCATION-FREE.  Fixed capacity, all storage inline.  Nothing here ever
//      calls the allocator, so it is safe to *query* from the audio callback.
//    * LOCK-FREE ON THE QUERY PATH.  resolve() takes no mutex and never blocks
//      on the writer.  Publication is RCU-style over rotating snapshot slots —
//      the same idiom PatchGraph already uses for its RenderPlan (kPlanSlots,
//      src/engine/patch/patch_graph.h:68) and MasterMixerNode uses for its
//      TrackSnap, so it is consistent with the tree rather than novel.
//    * O(fan-out) QUERY.  publish() buckets routes by source endpoint, so the
//      audio thread walks only the routes that actually leave the endpoint it
//      asked about, not all 512.
//
//  THREADING CONTRACT
//    Message thread (single writer):  addRoute / removeRoute / removeEndpoint /
//      setChannelMask / setMuted / clear, then ONE publish() to make the batch
//      visible.  Edits before publish() are invisible to readers — a half-built
//      routing change must never be audible.
//    Audio thread (many wait-free readers): resolve / isRouted / routeCount.
//
//    Publication uses kSlots rotating snapshots plus a per-slot seqlock stamp,
//    so a reader that is descheduled mid-scan detects the reuse and retries
//    instead of reading a torn table.  Retries are BOUNDED (kResolveRetries);
//    exhausting them is impossible at UI edit rates but is counted in
//    resolveRetryExhausted() rather than silently mis-routing.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_MIDI_MIDI_ROUTE_TABLE_H
#define PATCHKNOB_ENGINE_MIDI_MIDI_ROUTE_TABLE_H

#include <atomic>
#include <cstdint>
#include <cstring>

namespace PatchKnob { namespace engine { namespace midi {

// ---------------------------------------------------------------------------
// Endpoint — one addressable end of a MIDI wire.
//
// Every producer and consumer of MIDI in the engine gets a name here, so a
// route can say what it means instead of relying on an index convention.  The
// kinds map onto what exists in the tree today (see DESIGN.md §2).
// ---------------------------------------------------------------------------
enum class EndpointKind : uint8_t {
    None = 0,
    HardwareIn,    //!< an RtMidiIn port (audio_app.cpp: HwMidiIn)
    HardwareOut,   //!< an RtMidiOut port (audio_app.cpp: HwMidiOut)
    VirtualIn,     //!< VirtualMidiPortsNode public input
    VirtualOut,    //!< VirtualMidiPortsNode public output
    Sequencer,     //!< the pattern/arrangement scheduler (index = source track)
    TrackIn,       //!< MidiTrackNode "_live in" / "_playback in"
    TrackOut,      //!< MidiTrackNode "_track out"
    Instrument,    //!< a PluginNode MIDI input
    Recorder,      //!< track capture destination
    Count
};

struct Endpoint {
    EndpointKind kind  = EndpointKind::None;
    uint16_t     index = 0;      //!< which port/track/plug of that kind

    bool valid() const {
        return kind != EndpointKind::None && kind < EndpointKind::Count;
    }
    bool operator==(const Endpoint& o) const {
        return kind == o.kind && index == o.index;
    }
    bool operator!=(const Endpoint& o) const { return !(*this == o); }
};

inline Endpoint endpoint(EndpointKind k, int index) {
    Endpoint e; e.kind = k; e.index = (uint16_t)(index < 0 ? 0 : index); return e;
}

// Route flags.
enum : uint8_t {
    kRouteMuted = 0x01u    //!< kept in the table (so the UI remembers it) but not resolved
};

// ---------------------------------------------------------------------------
// Route — one directed MIDI connection, plus the per-connection transforms the
// engine already performs ad hoc today.
//
//   channelMask   bit c set => source channel c passes.  0xFFFF is omni.  This
//                 subsumes PluginNode::setMidiChannel()'s filter
//                 (src/engine/patch/patch_nodes.h) as route data instead of
//                 node state, so the same instrument can be fed by two routes
//                 on different channels.
//   channelForce  -1 passes the source channel through; 0..15 re-stamps every
//                 channel-voice message.  This is exactly what
//                 MidiInNode::setOutChannel() does for hardware input, hoisted
//                 out of the node and onto the wire where it belongs.
// ---------------------------------------------------------------------------
struct Route {
    Endpoint src{};
    Endpoint dst{};
    uint16_t channelMask  = 0xFFFFu;
    int8_t   channelForce = -1;
    uint8_t  flags        = 0;
};

//! What resolve() hands the audio thread: where to send it, and how to stamp it.
struct RouteTarget {
    Endpoint dst{};
    int8_t   channelForce = -1;
};

// ---------------------------------------------------------------------------
// RouteTableT — the table itself.
// ---------------------------------------------------------------------------
template <int MaxRoutes = 512, int Buckets = 128>
class RouteTableT {
public:
    static_assert(MaxRoutes > 0, "MaxRoutes must be positive");
    static_assert(Buckets > 0 && (Buckets & (Buckets - 1)) == 0,
                  "Buckets must be a power of two");

    static constexpr int kMaxRoutes      = MaxRoutes;
    static constexpr int kSlots          = 3;   //!< rotating snapshots (RCU grace)
    static constexpr int kResolveRetries = 4;

    RouteTableT() {
        for (int s = 0; s < kSlots; ++s) {
            slot_[s].count = 0;
            slot_[s].gen.store(0, std::memory_order_relaxed);
            for (int b = 0; b < Buckets; ++b) slot_[s].head[b] = -1;
        }
        publish();                     // an empty published table, never "no table"
    }

    RouteTableT(const RouteTableT&)            = delete;
    RouteTableT& operator=(const RouteTableT&) = delete;

    // =====================================================================
    // Message thread (single writer).  Edits stage into edit_; publish() makes
    // the whole batch visible at once.
    // =====================================================================

    //! Add a route.  Rejects invalid endpoints, a self-loop, an exact duplicate
    //! (same src+dst) and overflow.  Returns false without staging anything.
    bool addRoute(const Route& r) {
        if (!r.src.valid() || !r.dst.valid()) return false;
        if (r.src == r.dst)                   return false;
        if (editCount_ >= MaxRoutes)          return false;
        if (findEdit(r.src, r.dst) >= 0)      return false;
        edit_[editCount_++] = r;
        return true;
    }

    //! Convenience: an omni, pass-through route.
    bool connect(const Endpoint& src, const Endpoint& dst) {
        Route r; r.src = src; r.dst = dst; return addRoute(r);
    }

    bool removeRoute(const Endpoint& src, const Endpoint& dst) {
        const int i = findEdit(src, dst);
        if (i < 0) return false;
        edit_[i] = edit_[--editCount_];      // order within the table is not meaningful
        return true;
    }

    //! Drop every route that touches `e` — what a track/port teardown needs so a
    //! stale route can never point at a plug that no longer exists.  Returns the
    //! number removed.
    int removeEndpoint(const Endpoint& e) {
        int removed = 0;
        for (int i = editCount_ - 1; i >= 0; --i)
            if (edit_[i].src == e || edit_[i].dst == e) {
                edit_[i] = edit_[--editCount_];
                ++removed;
            }
        return removed;
    }

    void clear() { editCount_ = 0; }

    bool setChannelMask(const Endpoint& src, const Endpoint& dst, uint16_t mask) {
        const int i = findEdit(src, dst);
        if (i < 0) return false;
        edit_[i].channelMask = mask;
        return true;
    }

    bool setChannelForce(const Endpoint& src, const Endpoint& dst, int channel) {
        const int i = findEdit(src, dst);
        if (i < 0) return false;
        edit_[i].channelForce = (channel >= 0 && channel < 16) ? (int8_t)channel : (int8_t)-1;
        return true;
    }

    bool setMuted(const Endpoint& src, const Endpoint& dst, bool muted) {
        const int i = findEdit(src, dst);
        if (i < 0) return false;
        if (muted) edit_[i].flags = (uint8_t)(edit_[i].flags |  kRouteMuted);
        else       edit_[i].flags = (uint8_t)(edit_[i].flags & ~kRouteMuted);
        return true;
    }

    //! Freeze the staged edits into a snapshot and publish it to readers.
    void publish() {
        Snapshot& s = slot_[nextSlot_];
        const uint32_t g = s.gen.load(std::memory_order_relaxed);
        s.gen.store(g + 1, std::memory_order_relaxed);        // odd == being written
        std::atomic_thread_fence(std::memory_order_release);

        s.count = editCount_;
        if (editCount_ > 0)
            std::memcpy(s.route, edit_, sizeof(Route) * (size_t)editCount_);
        for (int b = 0; b < Buckets; ++b) s.head[b] = -1;
        // Walk backwards so each bucket chain comes out in ascending index order,
        // i.e. resolve() reports fan-out in the order the routes were added.
        for (int i = editCount_ - 1; i >= 0; --i) {
            const int b = bucketOf(s.route[i].src);
            s.next[i]  = s.head[b];
            s.head[b]  = i;
        }

        std::atomic_thread_fence(std::memory_order_release);
        s.gen.store(g + 2, std::memory_order_relaxed);        // even == readable
        active_.store(nextSlot_, std::memory_order_release);
        nextSlot_ = (nextSlot_ + 1) % kSlots;
    }

    //! Staged (unpublished) route count.
    int  pendingCount() const { return editCount_; }
    //! Staged routes, for a UI/serialiser that wants to enumerate them.
    const Route* pending() const { return edit_; }

    // =====================================================================
    // Audio thread (wait-free readers).
    // =====================================================================

    //! Fill `out` with every live destination of `src` for a message on
    //! `channel` (pass -1 for a system message that has no channel).  Returns
    //! the number written, never more than `cap`.
    int resolve(const Endpoint& src, int channel, RouteTarget* out, int cap) const {
        if (!out || cap <= 0 || !src.valid()) return 0;
        for (int attempt = 0; attempt < kResolveRetries; ++attempt) {
            const int a = active_.load(std::memory_order_acquire);
            if (a < 0 || a >= kSlots) return 0;
            const Snapshot& s = slot_[a];
            const uint32_t g0 = s.gen.load(std::memory_order_acquire);
            if (g0 & 1u) continue;                       // writer is inside this slot
            const int n = scan(s, src, channel, out, cap);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (s.gen.load(std::memory_order_relaxed) == g0) return n;   // clean read
        }
        // Only reachable if the writer published kResolveRetries times during one
        // scan, which cannot happen at UI edit rates.  Report nothing rather than
        // garbage, and make it observable instead of silent.
        retryExhausted_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    //! Is there a live (published, unmuted) route from src to dst?
    bool isRouted(const Endpoint& src, const Endpoint& dst) const {
        const int a = active_.load(std::memory_order_acquire);
        if (a < 0 || a >= kSlots) return false;
        const Snapshot& s = slot_[a];
        int guard = 0;
        for (int i = s.head[bucketOf(src)];
             i >= 0 && i < MaxRoutes && guard <= MaxRoutes; i = s.next[i], ++guard) {
            const Route& r = s.route[i];
            if (r.src == src && r.dst == dst && !(r.flags & kRouteMuted)) return true;
        }
        return false;
    }

    //! Published route count (including muted ones).
    int routeCount() const {
        const int a = active_.load(std::memory_order_acquire);
        return (a < 0 || a >= kSlots) ? 0 : slot_[a].count;
    }

    //! Diagnostics: how many resolve() calls gave up because of a writer storm.
    //! Must stay 0 in a healthy system; a non-zero value is a real bug report.
    uint32_t resolveRetryExhausted() const {
        return retryExhausted_.load(std::memory_order_relaxed);
    }

private:
    struct Snapshot {
        std::atomic<uint32_t> gen{0};      //!< seqlock stamp: odd == being written
        int   count = 0;
        Route route[MaxRoutes];
        int   head[Buckets];
        int   next[MaxRoutes];
    };

    static int bucketOf(const Endpoint& e) {
        // Cheap, well-spread mix; the endpoint space is tiny and dense.
        const uint32_t k = ((uint32_t)e.kind << 16) ^ (uint32_t)e.index;
        return (int)((k * 2654435761u) >> 16) & (Buckets - 1);
    }

    int findEdit(const Endpoint& src, const Endpoint& dst) const {
        for (int i = 0; i < editCount_; ++i)
            if (edit_[i].src == src && edit_[i].dst == dst) return i;
        return -1;
    }

    // Bucket walk.  `guard` bounds the loop even if a torn read hands us a
    // cyclic chain: the seqlock re-check in resolve() then throws the result
    // away, but we must not spin forever on the audio thread to get there.
    static int scan(const Snapshot& s, const Endpoint& src, int channel,
                    RouteTarget* out, int cap) {
        int n = 0, guard = 0;
        for (int i = s.head[bucketOf(src)];
             i >= 0 && i < MaxRoutes && n < cap && guard <= MaxRoutes;
             i = s.next[i], ++guard) {
            const Route& r = s.route[i];
            if (r.src != src)             continue;      // bucket collision
            if (r.flags & kRouteMuted)    continue;
            if (channel >= 0 && channel < 16 &&
                !(r.channelMask & (uint16_t)(1u << channel))) continue;
            out[n].dst          = r.dst;
            out[n].channelForce = r.channelForce;
            ++n;
        }
        return n;
    }

    // ---- message-thread-only staging area ----
    Route edit_[MaxRoutes];
    int   editCount_ = 0;
    int   nextSlot_  = 0;

    // ---- published snapshots ----
    Snapshot         slot_[kSlots];
    std::atomic<int> active_{-1};
    mutable std::atomic<uint32_t> retryExhausted_{0};
};

using RouteTable = RouteTableT<>;

}}} // namespace PatchKnob::engine::midi

#endif // PATCHKNOB_ENGINE_MIDI_MIDI_ROUTE_TABLE_H
