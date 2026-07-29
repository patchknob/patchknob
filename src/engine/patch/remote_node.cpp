//----------------------------------------------------------------------------
//  PatchKnob — RemoteNode / RemoteLink / netsock implementation.
//
//  See remote_node.h for the design. The layout of this file:
//    1. netsock  — the Winsock2 shim (POSIX branch is a drop-in).
//    2. RemoteLink — rings + jitter buffer + the net thread.
//    3. RemoteNode — the Node wrapper.
//----------------------------------------------------------------------------
#include "remote_node.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <mutex>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  // Windows-only UDP gotcha: an ICMP port-unreachable from a peer with no
  // listener makes later recvfrom() calls fail with WSAECONNRESET. We want
  // "peer silent" to look like silence (linkDown after 500 ms), not a socket
  // error storm, so the ioctl below turns that behaviour off.
  #ifndef SIO_UDP_CONNRESET
  #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
  #endif
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
#endif

namespace PatchKnob { namespace engine { namespace patch { namespace net {

// ===========================================================================
// 1. netsock — socket portability shim
// ===========================================================================
namespace netsock {

#ifdef _WIN32

// WSAStartup is process-global but must be balanced, so refcount it: several
// RemoteLinks (and the test's echo responder) share one startup.
static std::mutex g_wsaMutex;
static int        g_wsaRefs = 0;

bool startup() {
    std::lock_guard<std::mutex> lk(g_wsaMutex);
    if (g_wsaRefs == 0) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
    }
    ++g_wsaRefs;
    return true;
}

void cleanup() {
    std::lock_guard<std::mutex> lk(g_wsaMutex);
    if (g_wsaRefs > 0 && --g_wsaRefs == 0) WSACleanup();
}

socket_t openUdp() {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return kInvalidSocket;
    DWORD off = 0, ret = 0;
    ::WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ret,
               nullptr, nullptr);
    return (socket_t)s;
}

void closeSocket(socket_t s) {
    if (s != kInvalidSocket) ::closesocket((SOCKET)s);
}

bool setRecvTimeoutMs(socket_t s, int ms) {
    DWORD t = (DWORD)(ms < 1 ? 1 : ms);
    return ::setsockopt((SOCKET)s, SOL_SOCKET, SO_RCVTIMEO,
                        (const char*)&t, sizeof(t)) == 0;
}

#else // POSIX drop-in

bool startup()  { return true; }
void cleanup()  {}

socket_t openUdp() {
    int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    return s < 0 ? kInvalidSocket : (socket_t)s;
}

void closeSocket(socket_t s) {
    if (s != kInvalidSocket) ::close((int)s);
}

bool setRecvTimeoutMs(socket_t s, int ms) {
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (tv.tv_sec == 0 && tv.tv_usec == 0) tv.tv_usec = 1000;
    return ::setsockopt((int)s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

#endif

// The remaining calls are source-identical on both stacks (BSD sockets API).
// SOCKET is a UINT_PTR on Winsock and an int fd on POSIX; NSOCK() restores
// the native handle type without truncation.
#ifdef _WIN32
  #define NSOCK(s) ((SOCKET)(s))
#else
  #define NSOCK(s) ((int)(s))
#endif

bool resolve(const char* host, uint16_t port, Endpoint& out) {
    if (!host || !*host || port == 0) return false;
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    if (::getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
    const struct sockaddr_in* sin = (const struct sockaddr_in*)res->ai_addr;
    out.ip4  = ntohl(sin->sin_addr.s_addr);
    out.port = port;
    ::freeaddrinfo(res);
    return true;
}

bool bindLocal(socket_t s, uint16_t port) {
    struct sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port        = htons(port);
    return ::bind(NSOCK(s), (const struct sockaddr*)&a, sizeof(a)) == 0;
}

uint16_t boundPort(socket_t s) {
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    std::memset(&a, 0, sizeof(a));
    if (::getsockname(NSOCK(s), (struct sockaddr*)&a, &len) != 0) return 0;
    return ntohs(a.sin_port);
}

int sendTo(socket_t s, const void* buf, int len, const Endpoint& to) {
    struct sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(to.ip4);
    a.sin_port        = htons(to.port);
    return (int)::sendto(NSOCK(s), (const char*)buf, len, 0,
                         (const struct sockaddr*)&a, sizeof(a));
}

int recvFrom(socket_t s, void* buf, int cap, Endpoint* from) {
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    std::memset(&a, 0, sizeof(a));
    const int n = (int)::recvfrom(NSOCK(s), (char*)buf, cap, 0,
                                  (struct sockaddr*)&a, &len);
    if (n < 0) return -1;                    // timeout or transient error
    if (from) { from->ip4 = ntohl(a.sin_addr.s_addr); from->port = ntohs(a.sin_port); }
    return n;
}

} // namespace netsock

// ===========================================================================
// 2. RemoteLink
// ===========================================================================

static uint64_t thisThreadHash() {
    return (uint64_t)std::hash<std::thread::id>()(std::this_thread::get_id());
}

// Audio thread: remember who calls process() so the net thread can prove no
// socket call ever happens there.
void RemoteLink::noteProcessThread() {
    processThreadId_.store(thisThreadHash(), std::memory_order_relaxed);
}

// Net thread: count the syscall and verify the caller is NOT the process()
// thread. The atomic flag survives into release builds for the test; the
// assert makes a debug run stop dead at the violation.
void RemoteLink::dbgNoteSocketCall() {
    socketCalls_.fetch_add(1, std::memory_order_relaxed);
    const uint64_t pid = processThreadId_.load(std::memory_order_relaxed);
    if (pid != 0 && pid == thisThreadHash()) {
        socketOnProcessThread_.store(true, std::memory_order_relaxed);
        assert(!"RemoteLink: socket syscall on the audio (process) thread");
    }
}

bool RemoteLink::open(const Config& cfg) {
    close();

    if (cfg.blockFrames < 1 || cfg.blockFrames > 4096) return false;
    if (cfg.channels < 1 || cfg.channels > 8)          return false;
    if (cfg.peerHost.empty() || cfg.peerPort == 0)     return false;

    cfg_          = cfg;
    blockFloats_  = cfg.channels * cfg.blockFrames;
    payloadBytes_ = blockFloats_ * (int)sizeof(float);
    pktBytes_     = (int)sizeof(PacketHeader) + payloadBytes_;
    if (pktBytes_ > 1472) return false;      // must fit one MTU: never fragment

    if (!netsock::startup()) return false;
    if (!netsock::resolve(cfg.peerHost.c_str(), cfg.peerPort, peer_)) {
        netsock::cleanup();
        return false;
    }
    sock_ = netsock::openUdp();
    if (sock_ == netsock::kInvalidSocket) { netsock::cleanup(); return false; }
    if (!netsock::bindLocal(sock_, cfg.localPort)) {
        netsock::closeSocket(sock_);
        sock_ = netsock::kInvalidSocket;
        netsock::cleanup();
        return false;
    }
    // A 1 ms receive timeout keeps the net loop responsive to queued sends
    // and to stop_ without ever busy-spinning a core.
    netsock::setRecvTimeoutMs(sock_, 1);

    // Every RT-visible buffer is allocated HERE, on the message thread. The
    // audio thread and the running net thread never allocate.
    sendData_.assign((size_t)kSendRingBlocks * (size_t)blockFloats_, 0.0f);
    sendSeq_.assign((size_t)kSendRingBlocks, 0u);
    rxData_.assign((size_t)kJitterSlots * (size_t)blockFloats_, 0.0f);
    lastGood_.assign((size_t)blockFloats_, 0.0f);
    txScratch_.assign((size_t)pktBytes_, 0);
    rxScratch_.assign((size_t)pktBytes_ + 64, 0);
    for (int i = 0; i < kJitterSlots; ++i)
        rxTag_[i].store(0, std::memory_order_relaxed);
    sendHead_.store(0, std::memory_order_relaxed);
    sendTail_.store(0, std::memory_order_relaxed);
    haveLastGood_ = false;
    consecMiss_   = 0;
    linkDown_.store(false, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    sendDrops_.store(0, std::memory_order_relaxed);
    received_.store(0, std::memory_order_relaxed);
    badPackets_.store(0, std::memory_order_relaxed);
    socketCalls_.store(0, std::memory_order_relaxed);
    processThreadId_.store(0, std::memory_order_relaxed);
    socketOnProcessThread_.store(false, std::memory_order_relaxed);

    stop_.store(false, std::memory_order_relaxed);
    netThread_ = std::thread(&RemoteLink::netThreadLoop, this);
    open_.store(true, std::memory_order_release);
    return true;
}

void RemoteLink::close() {
    open_.store(false, std::memory_order_release);
    if (netThread_.joinable()) {
        stop_.store(true, std::memory_order_relaxed);
        netThread_.join();                   // returns within one recv timeout
    }
    if (sock_ != netsock::kInvalidSocket) {
        netsock::closeSocket(sock_);
        sock_ = netsock::kInvalidSocket;
        netsock::cleanup();
    }
}

bool RemoteLink::pushSend(const float* const* in, int channels, int nframes,
                          uint32_t seq) {
    noteProcessThread();
    if (!open_.load(std::memory_order_acquire)) return false;
    if (!in || nframes != cfg_.blockFrames)     return false;

    const uint32_t tail = sendTail_.load(std::memory_order_relaxed);
    const uint32_t head = sendHead_.load(std::memory_order_acquire);
    if (tail - head >= (uint32_t)kSendRingBlocks) {
        // Ring full (net thread stalled): DROP, never block the audio thread.
        sendDrops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const uint32_t slot = tail & (uint32_t)(kSendRingBlocks - 1);
    float* dst = &sendData_[(size_t)slot * (size_t)blockFloats_];
    const int copyCh = channels < cfg_.channels ? channels : cfg_.channels;
    for (int c = 0; c < cfg_.channels; ++c) {
        if (c < copyCh && in[c])
            std::memcpy(dst + (size_t)c * (size_t)nframes, in[c],
                        (size_t)nframes * sizeof(float));
        else
            std::memset(dst + (size_t)c * (size_t)nframes, 0,
                        (size_t)nframes * sizeof(float));
    }
    sendSeq_[slot] = seq;
    sendTail_.store(tail + 1, std::memory_order_release);
    return true;
}

bool RemoteLink::returnReady(uint32_t seq) const {
    const uint32_t slot = seq & (uint32_t)(kJitterSlots - 1);
    return rxTag_[slot].load(std::memory_order_acquire) == seq + 1;
}

static void zeroChans(float* const* out, int channels, int nframes) {
    for (int c = 0; c < channels; ++c)
        if (out[c]) std::memset(out[c], 0, (size_t)nframes * sizeof(float));
}

RemoteLink::Recv RemoteLink::pullReturn(float* const* out, int channels,
                                        int nframes, uint32_t seq) {
    noteProcessThread();
    if (!out) return Recv::LinkDown;
    if (!open_.load(std::memory_order_acquire) || nframes != cfg_.blockFrames) {
        zeroChans(out, channels, nframes);
        return Recv::LinkDown;
    }

    const uint32_t slot = seq & (uint32_t)(kJitterSlots - 1);
    if (rxTag_[slot].load(std::memory_order_acquire) == seq + 1) {
        const float* src = &rxData_[(size_t)slot * (size_t)blockFloats_];
        for (int c = 0; c < channels; ++c) {
            if (!out[c]) continue;
            if (c < cfg_.channels)
                std::memcpy(out[c], src + (size_t)c * (size_t)nframes,
                            (size_t)nframes * sizeof(float));
            else
                std::memset(out[c], 0, (size_t)nframes * sizeof(float));
        }
        // Keep a copy for concealment; a pre-sized memcpy, no allocation.
        std::memcpy(lastGood_.data(), src, (size_t)payloadBytes_);
        haveLastGood_ = true;
        consecMiss_   = 0;
        return Recv::Ok;
    }

    if (linkDown_.load(std::memory_order_relaxed)) {
        // Peer silent past the timeout: mute the return, keep the pipeline
        // running so recovery is a simple refill.
        zeroChans(out, channels, nframes);
        ++consecMiss_;
        return Recv::LinkDown;
    }

    // Miss: conceal. Repeat the last good block ONCE, then decay to silence
    // (repeating longer sounds like a stuck buzz).
    underruns_.fetch_add(1, std::memory_order_relaxed);
    if (consecMiss_++ == 0 && haveLastGood_) {
        for (int c = 0; c < channels; ++c) {
            if (!out[c]) continue;
            if (c < cfg_.channels)
                std::memcpy(out[c], lastGood_.data() + (size_t)c * (size_t)nframes,
                            (size_t)nframes * sizeof(float));
            else
                std::memset(out[c], 0, (size_t)nframes * sizeof(float));
        }
    } else {
        zeroChans(out, channels, nframes);
    }
    return Recv::Underrun;
}

void RemoteLink::netThreadLoop() {
    using clock = std::chrono::steady_clock;
    // Treat "just opened" as heard-from so a slow first handshake does not
    // flap linkDown before the peer ever had a chance to answer.
    clock::time_point lastRx = clock::now();

    while (!stop_.load(std::memory_order_relaxed)) {
        // ---- transmit: drain everything the audio thread queued -----------
        uint32_t       head = sendHead_.load(std::memory_order_relaxed);
        const uint32_t tail = sendTail_.load(std::memory_order_acquire);
        while (head != tail) {
            const uint32_t slot = head & (uint32_t)(kSendRingBlocks - 1);
            PacketHeader h;
            h.magic      = kMagic;
            h.version    = kVersion;
            h.flags      = 0;
            h.seq        = sendSeq_[slot];
            h.nframes    = (uint16_t)cfg_.blockFrames;
            h.channels   = (uint16_t)cfg_.channels;
            h.sampleRate = (uint32_t)(cfg_.sampleRate + 0.5);
            std::memcpy(txScratch_.data(), &h, sizeof(h));
            std::memcpy(txScratch_.data() + sizeof(h),
                        &sendData_[(size_t)slot * (size_t)blockFloats_],
                        (size_t)payloadBytes_);
            dbgNoteSocketCall();
            netsock::sendTo(sock_, txScratch_.data(), pktBytes_, peer_);
            ++head;
            sendHead_.store(head, std::memory_order_release);
        }

        // ---- receive: ONE bounded recv (1 ms timeout keeps the loop live) --
        dbgNoteSocketCall();
        const int n = netsock::recvFrom(sock_, rxScratch_.data(),
                                        (int)rxScratch_.size(), nullptr);
        const clock::time_point now = clock::now();
        if (n == pktBytes_) {
            PacketHeader h;
            std::memcpy(&h, rxScratch_.data(), sizeof(h));
            if (h.magic == kMagic && h.version == kVersion &&
                (int)h.nframes == cfg_.blockFrames &&
                (int)h.channels == cfg_.channels) {
                // Samples first, THEN the release-store of the tag: the pair
                // publishes the slot to the audio thread's acquire-load.
                const uint32_t slot = h.seq & (uint32_t)(kJitterSlots - 1);
                std::memcpy(&rxData_[(size_t)slot * (size_t)blockFloats_],
                            rxScratch_.data() + sizeof(h), (size_t)payloadBytes_);
                rxTag_[slot].store(h.seq + 1, std::memory_order_release);
                received_.fetch_add(1, std::memory_order_relaxed);
                lastRx = now;
                linkDown_.store(false, std::memory_order_relaxed);
            } else {
                badPackets_.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (n > 0) {
            badPackets_.fetch_add(1, std::memory_order_relaxed);
        }

        if (now - lastRx > std::chrono::milliseconds(kLinkDownMs))
            linkDown_.store(true, std::memory_order_relaxed);
    }
}

} // namespace net

// ===========================================================================
// 3. RemoteNode
// ===========================================================================

static void zeroBus(AudioBus* bus, int nframes) {
    if (!bus || !bus->chans) return;
    for (int c = 0; c < bus->channels; ++c)
        if (bus->chans[c])
            std::memset(bus->chans[c], 0, (size_t)nframes * sizeof(float));
}

RemoteNode::RemoteNode(int pipelineDepthBlocks, int jitterTargetBlocks) {
    setPipelineDepth(pipelineDepthBlocks);
    setJitterTarget(jitterTargetBlocks);
}

void RemoteNode::setEndpoint(const char* host, uint16_t port) {
    host_ = host ? host : "";
    port_ = port;
}

void RemoteNode::setPipelineDepth(int blocks) {
    // Half the jitter-slot count is the safe ceiling: the in-flight window
    // (D + arrival jitter) must never wrap the seq-indexed slot array.
    const int maxD = net::kJitterSlots / 2;
    depth_ = blocks < 1 ? 1 : (blocks > maxD ? maxD : blocks);
}

void RemoteNode::setJitterTarget(int blocks) {
    jitter_ = blocks < 1 ? 1 : blocks;
}

bool RemoteNode::prepare(double sampleRate, int maxBlock) {
    block_ = maxBlock;
    seq_   = 0;
    if (host_.empty() || port_ == 0) return false;
    net::RemoteLink::Config cfg;
    cfg.peerHost            = host_;
    cfg.peerPort            = port_;
    cfg.localPort           = 0;
    cfg.sampleRate          = sampleRate;
    cfg.blockFrames         = maxBlock;      // one datagram == one full block
    cfg.channels            = 2;
    cfg.pipelineDepthBlocks = depth_;
    cfg.jitterTargetBlocks  = jitter_;
    return link_.open(cfg);
}

void RemoteNode::release() {
    link_.close();
}

void RemoteNode::process(const NodeProcessContext& ctx) {
    AudioBus* out = (ctx.numAudioOut > 0 && ctx.audioOut) ? &ctx.audioOut[0]
                                                          : nullptr;
    if (!out) return;
    // A closed link — or a block size the wire framing was not opened for —
    // mutes the return rather than guessing (one datagram == one block_).
    if (!link_.isOpen() || ctx.nframes != block_) {
        zeroBus(out, ctx.nframes);
        return;
    }

    // (a) ship this block's input as sequence seq_ (lock-free ring push).
    if (ctx.numAudioIn > 0 && ctx.audioIn && ctx.audioIn[0].chans)
        link_.pushSend(ctx.audioIn[0].chans, ctx.audioIn[0].channels,
                       ctx.nframes, seq_);

    // (b) play the processed return due D blocks ago (lock-free jitter pop).
    if (seq_ >= (uint32_t)depth_)
        link_.pullReturn(out->chans, out->channels, ctx.nframes,
                         seq_ - (uint32_t)depth_);
    else
        zeroBus(out, ctx.nframes);           // warm-up: nothing can be due yet

    ++seq_;
}

}}} // namespace PatchKnob::engine::patch
