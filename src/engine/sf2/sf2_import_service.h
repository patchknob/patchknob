//----------------------------------------------------------------------------
//  src/engine/sf2/sf2_import_service.h -- asynchronous SoundFont preset
//  import: prepare on a worker thread, install on the message thread.
//
//  WHY.  Auditioning a preset out of a large bank costs 15-92 ms warm and up
//  to ~600 ms COLD (first touch of that preset's region on a ~150 MB/s
//  device).  That cold cost is the sequential-read floor of the disk -- it
//  cannot be made faster synchronously, so it must stop happening ON the
//  message thread.  preparePresetZones() (sf2_to_sampler.h) is exactly the
//  I/O + decode + intern part and touches no sampler; this service runs it on
//  one worker thread and hands the finished PreparedPreset back for the
//  message thread to install.
//
//  THREADING CONTRACT.
//   * begin() / cancel() / take() / progress() / prefetchNext(): MESSAGE
//     THREAD only.  (progress() is also safe from the worker internally.)
//   * The worker thread touches files and SoundFont structs.  It NEVER calls
//     a sampler setter -- installing the result is the caller's job, on the
//     message thread, via installPreparedPreset().
//   * Nothing here is ever called from, or blocks, the AUDIO thread, and no
//     lock in this file is ever taken by it.
//
//  SUPERSEDE / CANCEL ("click preset A, then B before A lands").
//  Every begin() gets a strictly increasing token and becomes "the newest
//  request"; the queue has depth ONE and latest wins.  The worker polls the
//  newest token between samples/zones and abandons a superseded prepare
//  within one sample-decode of the supersession.  A finished result is
//  delivered by take() ONLY if its token is still the newest -- a stale
//  result is destroyed, never handed out, so stale zones cannot reach the
//  sampler no matter how the clicks interleave.  take() runs on the same
//  thread as begin(), so "still the newest" cannot change between the check
//  and the install that follows it.
//
//  PREFETCH.  prefetchNext(font, bank, program) warms the page cache for the
//  preset AFTER (bank, program) in the bank's (sorted) preset list -- the one
//  the user is statistically about to audition next.  It reads raw bytes in
//  1 MB chunks and discards them (no decode, no allocation proportional to
//  the preset), runs only while no real request is pending, and re-checks
//  between chunks so a real click preempts it within ~one chunk read.
//  Measured on the 499 MB orchestral bank: a prefetched preset's cold
//  prepare drops from ~600 ms to warm speed; see the perf report.
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_SF2_IMPORT_SERVICE_H
#define PATCHKNOB_ENGINE_SF2_IMPORT_SERVICE_H

#include "sf2_to_sampler.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace PatchKnob { namespace engine { namespace sf2 {

class Sf2ImportService {
public:
    Sf2ImportService();
    ~Sf2ImportService();                       //!< cancels + joins the worker
    Sf2ImportService(const Sf2ImportService&) = delete;
    Sf2ImportService& operator=(const Sf2ImportService&) = delete;

    //! Start preparing (bank, program) of `fontPath` in the background,
    //! superseding any request still in flight.  Returns the request's token.
    //! Message thread only.
    uint64_t begin(const std::string& fontPath, int bank, int program,
                   const std::string& label, const ImportOptions& opts = {});

    //! Abandon whatever is in flight or finished-but-untaken.  Message thread.
    void cancel();

    //! Live progress of the newest request (fraction = samplesDone/samplesTotal
    //! once samplesTotal > 0).  Message thread.
    struct Progress {
        bool        active = false;   //!< a request is queued or being prepared
        uint64_t    token = 0;        //!< which request the counters describe
        int         samplesDone = 0;
        int         samplesTotal = 0;
        std::string label;
    };
    Progress progress() const;

    //! A finished prepare, ready to install.  `prepared` is only meaningful
    //! when ok; on !ok, `error` says why (parse failure, preset not found...).
    struct Completed {
        uint64_t       token = 0;
        bool           ok = false;
        std::string    error;
        std::string    fontPath;
        std::string    label;
        int            bank = 0, program = 0;
        int            presetIndex = -1;      //!< resolved index within the font
        PreparedPreset prepared;
    };

    //! Deliver the finished result IF it is still the newest request; a stale
    //! result is destroyed instead and false is returned.  Message thread.
    //! Poll once per frame; install the delivered result immediately (same
    //! thread, so no newer begin() can slip in between).
    bool take(Completed& out);

    //! True when nothing is queued, being prepared, or being prefetched.
    //! (A finished-but-untaken result does not count as busy.)  For tests
    //! and for gating shutdown; any thread.
    bool idle() const;

    //! Queue a page-cache warm of the preset after (bank, program) in
    //! `fontPath`.  Lowest priority; silently dropped/preempted by real work.
    //! Message thread.
    void prefetchNext(const std::string& fontPath, int bank, int program);

private:
    struct Request {
        uint64_t      token = 0;
        std::string   fontPath;
        int           bank = 0, program = 0;
        std::string   label;
        ImportOptions opts;
    };
    struct PrefetchReq { std::string fontPath; int bank = 0, program = 0; };

    void workerMain();
    void runPrepare(Request req);              //!< worker
    void runPrefetch(const PrefetchReq& pf);   //!< worker

    // m_mx guards everything below it EXCEPT the atomics.  Held only for
    // short hand-offs (never across I/O), and only by the message thread and
    // the worker -- the audio thread has no path into this class.
    mutable std::mutex      m_mx;
    std::condition_variable m_cv;
    bool                    m_quit = false;
    bool                    m_hasPending = false;
    Request                 m_pending;
    bool                    m_hasResult = false;
    Completed               m_result;
    std::deque<PrefetchReq> m_prefetch;
    std::string             m_label;            //!< of the newest request
    uint64_t                m_nextToken = 1;

    //! Token of the NEWEST request -- the supersede/cancel signal.  Atomic so
    //! the worker's per-sample poll (inside the prepare progress callback)
    //! costs a relaxed load, not a lock acquisition per decoded sample.
    std::atomic<uint64_t>   m_newest{0};
    std::atomic<int>        m_done{0}, m_total{0};
    std::atomic<bool>       m_busy{false};      //!< worker is preparing/prefetching

    std::thread             m_worker;
};

}}} // namespace PatchKnob::engine::sf2

#endif // PATCHKNOB_ENGINE_SF2_IMPORT_SERVICE_H
