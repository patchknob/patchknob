//----------------------------------------------------------------------------
//  PatchKnob — DISTRIBUTED patch graph, increment 1: RemoteNode.
//
//  RemoteNode ships its (summed) stereo audio-in to a remote processor over
//  UDP and plays the processed return back with a FIXED pipeline delay of D
//  blocks, reported via latencySamples() so PDC can line dry/parallel paths
//  up. In the master graph it looks like any other insert node:
//
//      audio in (stereo) ─► [ the wire ] ─► audio out (stereo, D blocks late)
//
//  The one rule the whole design hangs on: THE AUDIO THREAD NEVER TOUCHES A
//  SOCKET. sendto/recvfrom are blocking syscalls with unbounded tail latency,
//  so process() only
//    (a) copies this block's input into a lock-free SPSC send ring, tagged
//        with sequence N, and
//    (b) copies the already-arrived processed block for sequence N-D out of
//        a seq-indexed jitter buffer into audio-out.
//  RemoteLink's dedicated NET THREAD does every syscall. A lost or late
//  return block is CONCEALED (repeat the last good block once, then silence)
//  and counted in atomics; a peer silent for >500 ms flips an atomic
//  linkDown flag — the node outputs silence while the net thread keeps
//  listening for recovery. process() never blocks, never allocates, never
//  syscalls (debug-asserted by thread-id capture, see dbg accessors below).
//
//  Wire format: ONE datagram == ONE audio block. A 64-frame stereo float
//  block is 512 B payload + 20 B header — always inside one MTU, never
//  fragmented. Fields are little-endian on the wire (every target is LE).
//
//  Sockets sit behind the tiny netsock shim so a POSIX build is a drop-in
//  later; on Windows it is Winsock2/ws2_32 with a refcounted WSAStartup.
//
//  NOT in this increment (see the distributed-architecture notes): ASRC /
//  clock-drift compensation, discovery/handshake, redundancy, MIDI over the
//  link, and the Pi-side RemoteSource/RemoteSink worker nodes.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_PATCH_REMOTE_NODE_H
#define PATCHKNOB_ENGINE_PATCH_REMOTE_NODE_H

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "patch_graph.h"

namespace PatchKnob { namespace engine { namespace patch { namespace net {

// ---------------------------------------------------------------------------
// netsock — the socket portability shim. Winsock2 today; a POSIX build only
// reimplements these nine functions. All calls are made on message/net
// threads, NEVER on the audio thread.
// ---------------------------------------------------------------------------
namespace netsock {

using socket_t = uintptr_t;                     // SOCKET on Win32, fd on POSIX
constexpr socket_t kInvalidSocket = ~(socket_t)0;

struct Endpoint { uint32_t ip4 = 0; uint16_t port = 0; };   // host byte order

bool     startup();                             // refcounted WSAStartup (no-op on POSIX)
void     cleanup();
bool     resolve(const char* host, uint16_t port, Endpoint& out);
socket_t openUdp();
void     closeSocket(socket_t s);
bool     bindLocal(socket_t s, uint16_t port);  // 0 == ephemeral
uint16_t boundPort(socket_t s);                 // valid after bindLocal
bool     setRecvTimeoutMs(socket_t s, int ms);
int      sendTo(socket_t s, const void* buf, int len, const Endpoint& to);
//! Bytes received, or -1 on timeout/error (never blocks past the configured
//! receive timeout).
int      recvFrom(socket_t s, void* buf, int cap, Endpoint* from);

} // namespace netsock

// ---- wire protocol ----------------------------------------------------------
constexpr uint32_t kMagic   = 0x53524E31;       // "SRN1"
constexpr uint16_t kVersion = 1;

#pragma pack(push, 1)
struct PacketHeader {           // 20 bytes; payload = planar float32 [ch][nframes]
    uint32_t magic;             // kMagic
    uint16_t version;           // kVersion
    uint16_t flags;             // reserved (redundancy / silence flags later)
    uint32_t seq;               // monotonic per link; the jitter-buffer key
    uint16_t nframes;           // frames in the payload (== link blockFrames)
    uint16_t channels;          // channels in the payload
    uint32_t sampleRate;        // Hz; sanity check only (fixed per link)
};
#pragma pack(pop)
static_assert(sizeof(PacketHeader) == 20, "PacketHeader must pack to 20 bytes");

// ---- fixed capacities (sized so the in-flight seq window can never collide) --
constexpr int kSendRingBlocks   = 128;   // SPSC audio->net ring depth
constexpr int kJitterSlots      = 64;    // seq-indexed return slots
constexpr int kLinkDownMs       = 500;   // peer silent this long -> linkDown
static_assert((kSendRingBlocks & (kSendRingBlocks - 1)) == 0, "power of two");
static_assert((kJitterSlots    & (kJitterSlots    - 1)) == 0, "power of two");

// ---------------------------------------------------------------------------
// RemoteLink — owns ONE UDP socket + the background net thread. The audio
// thread touches ONLY the lock-free rings (pushSend / pullReturn /
// returnReady); the net thread makes every syscall. open()/close() are
// message-thread only and must not race a running audio thread (the usual
// prepare()/release() contract).
// ---------------------------------------------------------------------------
class RemoteLink {
public:
    struct Config {
        std::string peerHost;                // where the remote processor listens
        uint16_t    peerPort  = 0;
        uint16_t    localPort = 0;           // 0 == ephemeral
        double      sampleRate  = 48000.0;
        int         blockFrames = 64;        // ONE datagram == ONE such block
        int         channels    = 2;
        int         pipelineDepthBlocks = 2; // D: node latency = D * blockFrames
        int         jitterTargetBlocks  = 1; // steady-state depth (informational
                                             // until the ASRC increment lands)
    };

    RemoteLink() = default;
    ~RemoteLink() { close(); }
    RemoteLink(const RemoteLink&)            = delete;
    RemoteLink& operator=(const RemoteLink&) = delete;

    // --- lifecycle (message thread) ---
    bool open(const Config& cfg);            // bind + spawn the net thread
    void close();                            // join the net thread, free the socket
    bool isOpen() const { return open_.load(std::memory_order_acquire); }

    // --- audio thread only (lock-free, allocation-free, no syscalls) ---
    //! Enqueue this block's input for transmit as sequence `seq`. A full ring
    //! DROPS the block (counted) rather than ever blocking.
    bool pushSend(const float* const* in, int channels, int nframes, uint32_t seq);
    //! Fetch the processed return due for `seq` into `out`. On a miss the
    //! output is CONCEALED (repeat-last-block once, then silence) and counted;
    //! when the link is down the output is silence.
    enum class Recv { Ok, Underrun, LinkDown };
    Recv pullReturn(float* const* out, int channels, int nframes, uint32_t seq);
    //! True once the return block for `seq` has arrived (atomic peek; safe
    //! from any thread — the test harness paces itself on this).
    bool returnReady(uint32_t seq) const;

    // --- message thread / UI (atomics) ---
    bool     linkDown()   const { return linkDown_.load(std::memory_order_relaxed); }
    uint64_t underruns()  const { return underruns_.load(std::memory_order_relaxed); }
    uint64_t sendDrops()  const { return sendDrops_.load(std::memory_order_relaxed); }
    uint64_t received()   const { return received_.load(std::memory_order_relaxed); }
    uint64_t badPackets() const { return badPackets_.load(std::memory_order_relaxed); }

    // --- debug proof that no socket call happens on the process() thread ----
    // pushSend/pullReturn record their caller's thread id; every socket call
    // on the net thread checks against it (and asserts in debug builds).
    uint64_t socketCalls() const { return socketCalls_.load(std::memory_order_relaxed); }
    bool     socketCalledOnProcessThread() const
                { return socketOnProcessThread_.load(std::memory_order_relaxed); }

private:
    void netThreadLoop();                    // ALL sendto/recvfrom happen here
    void noteProcessThread();                // audio thread: record caller tid
    void dbgNoteSocketCall();                // net thread: count + tid check

    Config            cfg_;
    netsock::socket_t sock_ = netsock::kInvalidSocket;
    netsock::Endpoint peer_{};
    std::thread       netThread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> open_{false};

    int blockFloats_  = 0;                   // channels * blockFrames
    int payloadBytes_ = 0;
    int pktBytes_     = 0;

    // Send ring: SPSC, audio thread produces (tail), net thread consumes (head).
    std::vector<float>    sendData_;         // kSendRingBlocks * blockFloats_
    std::vector<uint32_t> sendSeq_;
    std::atomic<uint32_t> sendHead_{0};
    std::atomic<uint32_t> sendTail_{0};

    // Jitter buffer: slot = seq % kJitterSlots. The net thread writes the
    // samples THEN release-stores tag = seq+1 (0 == empty); the audio thread
    // acquire-loads the tag before copying. The in-flight window (D + jitter,
    // a handful of blocks) is far below kJitterSlots, so a slot is never
    // rewritten while its previous occupant is still due.
    std::vector<float>    rxData_;           // kJitterSlots * blockFloats_
    std::atomic<uint32_t> rxTag_[kJitterSlots];
    std::vector<float>    lastGood_;         // concealment source (audio thread only)
    bool                  haveLastGood_ = false;
    int                   consecMiss_   = 0;

    // Net-thread scratch (pre-sized in open(); the net thread never allocates
    // either once running).
    std::vector<uint8_t>  txScratch_;
    std::vector<uint8_t>  rxScratch_;

    std::atomic<bool>     linkDown_{false};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> sendDrops_{0};
    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> badPackets_{0};
    std::atomic<uint64_t> socketCalls_{0};
    std::atomic<uint64_t> processThreadId_{0};
    std::atomic<bool>     socketOnProcessThread_{false};
};

} // namespace net

// ---------------------------------------------------------------------------
// RemoteNode — the master-side graph node wrapping a RemoteLink. Stereo audio
// in -> UDP send; jitter-buffered UDP return -> stereo audio out, D blocks
// late (reported via latencySamples() for PDC). Configure with setEndpoint()
// / setPipelineDepth() on the message thread BEFORE prepare().
// ---------------------------------------------------------------------------
class RemoteNode : public Node {
public:
    explicit RemoteNode(int pipelineDepthBlocks = 2, int jitterTargetBlocks = 1);

    const char* typeName() const override { return "RemoteNode"; }
    int      numPorts() const override { return 2; }
    PortDesc port(int i) const override {
        return (i == 0) ? PortDesc{ 0, PortKind::Audio, PortDir::In,  2, "in"  }
                        : PortDesc{ 1, PortKind::Audio, PortDir::Out, 2, "out" };
    }

    // Message thread, before prepare(): where the remote processor listens.
    void setEndpoint(const char* host, uint16_t port);
    // D blocks of pipeline delay (clamped to [1, kJitterSlots/2]); the return
    // for block N plays at block N+D. Takes effect at the next prepare().
    void setPipelineDepth(int blocks);
    // Steady-state receive-buffer depth target (informational this increment).
    void setJitterTarget(int blocks);

    bool prepare(double sampleRate, int maxBlock) override;   // sizes rings, opens link
    void release() override;                                  // closes link, joins net thread
    void process(const NodeProcessContext& ctx) override;     // RT: rings only, no syscalls

    // Reported so PatchGraph PDC can delay dry/parallel paths to match.
    int latencySamples() const override { return depth_ * block_; }

    // Message thread / UI: linkDown() / underruns() / counters live here.
    net::RemoteLink&       link()       { return link_; }
    const net::RemoteLink& link() const { return link_; }

private:
    net::RemoteLink link_;
    std::string     host_;
    uint16_t        port_   = 0;
    uint32_t        seq_    = 0;   // bumped once per process() block
    int             depth_  = 2;
    int             jitter_ = 1;
    int             block_  = 0;
};

}}} // namespace PatchKnob::engine::patch

#endif // PATCHKNOB_ENGINE_PATCH_REMOTE_NODE_H
