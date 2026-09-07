//----------------------------------------------------------------------------
//  PatchKnob — midi_route_table.h self-test.
//
//  Same lightweight CHECK harness as src/engine/patch/patch_nodes_test.cpp; no
//  test framework, no dependencies beyond the header under test.
//
//  What is pinned here:
//    1.  A fresh table resolves nothing and reports no routes.
//    2.  Fan-out: one source to many destinations, reported in insertion order.
//    3.  Staged edits are INVISIBLE until publish() — a half-built routing
//        change must never be audible.
//    4.  Rejection rules: invalid endpoint, self-loop, exact duplicate, overflow.
//    5.  removeRoute / removeEndpoint (the teardown a vanishing track needs).
//    6.  Channel mask filtering and channelForce.
//    7.  Mute keeps the route but stops resolving it.
//    8.  Bucket collisions never leak another endpoint's routes.
//    9.  Capacity: fill to kMaxRoutes and still resolve correctly.
//   10.  Concurrency: readers resolving while the writer republishes see one of
//        the two legal configurations and never a mix, never over cap, never a
//        hang, and never an exhausted retry budget.
//----------------------------------------------------------------------------
#include "midi_route_table.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace PatchKnob::engine::midi;

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);        \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static Endpoint hwIn(int i)   { return endpoint(EndpointKind::HardwareIn,  i); }
static Endpoint hwOut(int i)  { return endpoint(EndpointKind::HardwareOut, i); }
static Endpoint instr(int i)  { return endpoint(EndpointKind::Instrument,  i); }
static Endpoint seqSrc(int i) { return endpoint(EndpointKind::Sequencer,   i); }

// ===========================================================================
// 1. Empty table.
// ===========================================================================
static void test_empty() {
    RouteTable rt;
    RouteTarget t[8];
    CHECK(rt.routeCount() == 0, "empty: no published routes");
    CHECK(rt.resolve(hwIn(0), 0, t, 8) == 0, "empty: resolves nothing");
    CHECK(!rt.isRouted(hwIn(0), instr(0)), "empty: isRouted false");
    CHECK(rt.resolve(Endpoint{}, 0, t, 8) == 0, "empty: an invalid source resolves nothing");
    CHECK(rt.resolve(hwIn(0), 0, nullptr, 8) == 0, "empty: a null sink resolves nothing");
    CHECK(rt.resolve(hwIn(0), 0, t, 0) == 0, "empty: zero capacity resolves nothing");
}

// ===========================================================================
// 2. Fan-out, in insertion order.  This is the case the implicit
//    "bus == track == plug" convention cannot express at all.
// ===========================================================================
static void test_fanout() {
    RouteTable rt;
    CHECK(rt.connect(hwIn(0), instr(3)), "fanout: connect #1");
    CHECK(rt.connect(hwIn(0), instr(7)), "fanout: connect #2");
    CHECK(rt.connect(hwIn(0), hwOut(1)), "fanout: connect #3");
    CHECK(rt.connect(hwIn(1), instr(9)), "fanout: a second, unrelated source");
    rt.publish();

    RouteTarget t[8];
    const int n = rt.resolve(hwIn(0), 0, t, 8);
    CHECK(n == 3, "fanout: one keyboard drives three destinations");
    CHECK(n == 3 && t[0].dst == instr(3) && t[1].dst == instr(7) && t[2].dst == hwOut(1),
          "fanout: destinations come back in insertion order");
    CHECK(rt.resolve(hwIn(1), 0, t, 8) == 1 && t[0].dst == instr(9),
          "fanout: the other source resolves only its own route");
    CHECK(rt.routeCount() == 4, "fanout: published count");
    CHECK(rt.isRouted(hwIn(0), instr(7)), "fanout: isRouted true for a live route");
    CHECK(!rt.isRouted(hwIn(1), instr(7)), "fanout: isRouted false across sources");

    // Truncation is bounded by cap, never a buffer overrun.
    RouteTarget small[2];
    CHECK(rt.resolve(hwIn(0), 0, small, 2) == 2, "fanout: resolve respects cap");
}

// ===========================================================================
// 3. Edits are invisible until publish().
// ===========================================================================
static void test_publish_barrier() {
    RouteTable rt;
    rt.connect(hwIn(0), instr(0));
    CHECK(rt.pendingCount() == 1, "publish barrier: the edit is staged");
    CHECK(rt.routeCount() == 0,   "publish barrier: but not published");
    RouteTarget t[4];
    CHECK(rt.resolve(hwIn(0), 0, t, 4) == 0,
          "publish barrier: readers cannot see a half-built change");
    rt.publish();
    CHECK(rt.routeCount() == 1, "publish barrier: publish() commits it");
    CHECK(rt.resolve(hwIn(0), 0, t, 4) == 1, "publish barrier: now resolvable");

    // A batch commits atomically: two adds and a remove become visible together.
    rt.connect(hwIn(0), instr(1));
    rt.connect(hwIn(0), instr(2));
    rt.removeRoute(hwIn(0), instr(0));
    CHECK(rt.resolve(hwIn(0), 0, t, 4) == 1,
          "publish barrier: the whole batch is still invisible");
    rt.publish();
    CHECK(rt.resolve(hwIn(0), 0, t, 4) == 2, "publish barrier: the batch lands at once");
    CHECK(!rt.isRouted(hwIn(0), instr(0)), "publish barrier: the removal landed too");
}

// ===========================================================================
// 4. Rejection rules.
// ===========================================================================
static void test_rejects() {
    RouteTable rt;
    CHECK(!rt.connect(Endpoint{}, instr(0)), "reject: invalid source");
    CHECK(!rt.connect(hwIn(0), Endpoint{}),  "reject: invalid destination");
    CHECK(!rt.connect(hwIn(0), hwIn(0)),     "reject: self-loop");
    CHECK(rt.connect(hwIn(0), instr(0)),     "reject: the good one is accepted");
    CHECK(!rt.connect(hwIn(0), instr(0)),    "reject: exact duplicate");
    CHECK(rt.pendingCount() == 1, "reject: nothing was staged by a rejected add");

    // A duplicate must not double-deliver: that is how a note plays twice and
    // one of the two note-offs looks spurious.
    rt.publish();
    RouteTarget t[4];
    CHECK(rt.resolve(hwIn(0), 0, t, 4) == 1, "reject: no double delivery");

    // Overflow: fill to capacity, then one more.
    RouteTableT<8, 8> small;
    for (int i = 0; i < 8; ++i)
        CHECK(small.connect(hwIn(0), instr(i)), "reject: fills to capacity");
    CHECK(!small.connect(hwIn(0), instr(99)), "reject: overflow refused");
    CHECK(small.pendingCount() == 8, "reject: overflow staged nothing");
}

// ===========================================================================
// 5. Teardown.  A stale route pointing at a port that no longer exists is
//    exactly how MIDI ends up delivered to the wrong instrument.
// ===========================================================================
static void test_teardown() {
    RouteTable rt;
    rt.connect(seqSrc(0), instr(0));
    rt.connect(seqSrc(1), instr(1));
    rt.connect(hwIn(0),   instr(1));
    rt.connect(instr(1),  hwOut(0));
    rt.publish();
    CHECK(rt.routeCount() == 4, "teardown: four routes to start");

    CHECK(rt.removeRoute(seqSrc(0), instr(0)), "teardown: removeRoute hits");
    CHECK(!rt.removeRoute(seqSrc(0), instr(0)), "teardown: removeRoute is idempotent");
    rt.publish();
    CHECK(rt.routeCount() == 3, "teardown: the route is gone");

    // Instrument 1 is deleted: every route touching it, in either direction, dies.
    const int n = rt.removeEndpoint(instr(1));
    CHECK(n == 3, "teardown: removeEndpoint drops sources AND destinations");
    rt.publish();
    CHECK(rt.routeCount() == 0, "teardown: nothing points at the dead endpoint");
    RouteTarget t[4];
    CHECK(rt.resolve(hwIn(0), 0, t, 4) == 0, "teardown: the orphaned source resolves nothing");
}

// ===========================================================================
// 6. Channel mask + channelForce.
// ===========================================================================
static void test_channels() {
    RouteTable rt;
    Route a; a.src = hwIn(0); a.dst = instr(0);
    a.channelMask  = (uint16_t)(1u << 0);          // only channel 0
    a.channelForce = 5;                            // re-stamped onto channel 5
    CHECK(rt.addRoute(a), "channels: add masked route");

    Route b; b.src = hwIn(0); b.dst = instr(1);
    b.channelMask = (uint16_t)((1u << 9) | (1u << 10));
    CHECK(rt.addRoute(b), "channels: add drum-channel route");
    rt.publish();

    RouteTarget t[4];
    int n = rt.resolve(hwIn(0), 0, t, 4);
    CHECK(n == 1 && t[0].dst == instr(0) && t[0].channelForce == 5,
          "channels: channel 0 reaches only the masked route, re-stamped");
    n = rt.resolve(hwIn(0), 9, t, 4);
    CHECK(n == 1 && t[0].dst == instr(1) && t[0].channelForce == -1,
          "channels: channel 9 reaches only the drum route, passthrough");
    n = rt.resolve(hwIn(0), 3, t, 4);
    CHECK(n == 0, "channels: an unmasked channel reaches nobody");

    // A system message has no channel: -1 bypasses the mask entirely, so clock
    // and panic messages always reach every destination.
    n = rt.resolve(hwIn(0), -1, t, 4);
    CHECK(n == 2, "channels: channel -1 (system message) is never masked out");

    CHECK(rt.setChannelForce(hwIn(0), instr(1), 2), "channels: setChannelForce hits");
    CHECK(!rt.setChannelForce(hwIn(0), instr(9), 2), "channels: miss returns false");
    rt.publish();
    n = rt.resolve(hwIn(0), 10, t, 4);
    CHECK(n == 1 && t[0].channelForce == 2, "channels: channelForce updated");
    CHECK(rt.setChannelForce(hwIn(0), instr(1), 99), "channels: out-of-range accepted...");
    rt.publish();
    n = rt.resolve(hwIn(0), 10, t, 4);
    CHECK(n == 1 && t[0].channelForce == -1, "channels: ...and normalised to passthrough");
}

// ===========================================================================
// 7. Mute.
// ===========================================================================
static void test_mute() {
    RouteTable rt;
    rt.connect(hwIn(0), instr(0));
    rt.connect(hwIn(0), instr(1));
    rt.publish();
    RouteTarget t[4];
    CHECK(rt.resolve(hwIn(0), 0, t, 4) == 2, "mute: both live to start");

    CHECK(rt.setMuted(hwIn(0), instr(0), true), "mute: setMuted hits");
    rt.publish();
    CHECK(rt.resolve(hwIn(0), 0, t, 4) == 1 && t[0].dst == instr(1),
          "mute: a muted route stops resolving");
    CHECK(!rt.isRouted(hwIn(0), instr(0)), "mute: isRouted reports it dead");
    CHECK(rt.routeCount() == 2, "mute: but the route is REMEMBERED, not deleted");

    CHECK(rt.setMuted(hwIn(0), instr(0), false), "mute: unmute");
    rt.publish();
    CHECK(rt.resolve(hwIn(0), 0, t, 4) == 2, "mute: unmuting restores it");
}

// ===========================================================================
// 8. Bucket collisions never leak another endpoint's routes.
// ===========================================================================
static void test_bucket_isolation() {
    RouteTable rt;
    const int kSrcs = 200;                        // >> the 128 buckets: collisions forced
    for (int i = 0; i < kSrcs; ++i)
        CHECK(rt.connect(seqSrc(i), instr(i)), "buckets: connect");
    rt.publish();
    CHECK(rt.routeCount() == kSrcs, "buckets: all published");

    bool clean = true;
    RouteTarget t[8];
    for (int i = 0; i < kSrcs; ++i) {
        const int n = rt.resolve(seqSrc(i), 0, t, 8);
        if (n != 1 || !(t[0].dst == instr(i))) clean = false;
    }
    CHECK(clean, "buckets: every source resolves to exactly its own destination");
    CHECK(rt.resolve(seqSrc(kSrcs + 1), 0, t, 8) == 0,
          "buckets: an unrouted source in a busy bucket resolves nothing");
    // Endpoints of different KINDS at the same index must never be confused.
    CHECK(rt.resolve(hwIn(5), 0, t, 8) == 0,
          "buckets: kind is part of the identity, not just the index");
}

// ===========================================================================
// 9. Full capacity.
// ===========================================================================
static void test_capacity() {
    RouteTableT<64, 16> rt;
    for (int i = 0; i < 64; ++i)
        CHECK(rt.connect(hwIn(0), instr(i)), "capacity: fill");
    CHECK(!rt.connect(hwIn(0), instr(64)), "capacity: refuse past the end");
    rt.publish();
    CHECK(rt.routeCount() == 64, "capacity: all 64 published");
    RouteTarget t[64];
    const int n = rt.resolve(hwIn(0), 0, t, 64);
    CHECK(n == 64, "capacity: full fan-out resolves");
    bool ordered = (n == 64);
    for (int i = 0; ordered && i < n; ++i)
        if (!(t[i].dst == instr(i))) ordered = false;
    CHECK(ordered, "capacity: order preserved at full capacity");
}

// ===========================================================================
// 10. Readers vs a republishing writer.
//
//     The writer alternates between two legal configurations.  A reader must
//     always see exactly one of them — never a blend, never a stale destination
//     that belongs to neither, never more than cap, and never a hang.
// ===========================================================================
static void test_concurrent_publish() {
    RouteTable rt;
    rt.connect(hwIn(0), instr(1));
    rt.publish();

    std::atomic<bool> run{true};
    std::atomic<int>  illegal{0};      // a destination that is in neither config
    std::atomic<int>  overCap{0};
    std::atomic<long> reads{0};

    auto reader = [&] {
        RouteTarget t[4];
        while (run.load(std::memory_order_relaxed)) {
            const int n = rt.resolve(hwIn(0), 0, t, 4);
            if (n > 4 || n < 0) { overCap.fetch_add(1); continue; }
            for (int i = 0; i < n; ++i)
                if (!(t[i].dst == instr(1)) && !(t[i].dst == instr(2)))
                    illegal.fetch_add(1, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread r1(reader), r2(reader);

    for (int k = 0; k < 20000; ++k) {
        rt.clear();
        if (k & 1) rt.connect(hwIn(0), instr(1));
        else       rt.connect(hwIn(0), instr(2));
        rt.publish();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    run.store(false);
    r1.join(); r2.join();

    CHECK(illegal.load() == 0, "concurrent: a reader never sees a destination from no config");
    CHECK(overCap.load() == 0, "concurrent: resolve never exceeds cap");
    CHECK(reads.load() > 0,    "concurrent: readers actually ran");
    CHECK(rt.resolveRetryExhausted() == 0,
          "concurrent: no reader ever ran out of retries (three slots are enough)");

    // Quiesced, the content is exactly the last published configuration.
    RouteTarget t[4];
    const int n = rt.resolve(hwIn(0), 0, t, 4);
    CHECK(n == 1 && t[0].dst == instr(2),
          "concurrent: the final state is the last publish, intact");
}

// ===========================================================================
int main() {
    test_empty();
    test_fanout();
    test_publish_barrier();
    test_rejects();
    test_teardown();
    test_channels();
    test_mute();
    test_bucket_isolation();
    test_capacity();
    test_concurrent_publish();

    if (g_failures == 0) { std::printf("midi_route_table_test: ALL PASS\n"); return 0; }
    std::printf("midi_route_table_test: %d FAILURE(S)\n", g_failures);
    return 1;
}
