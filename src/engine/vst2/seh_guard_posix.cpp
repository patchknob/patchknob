//----------------------------------------------------------------------------
//  PatchKnob (ZERO-JUCE) — POSIX fault guard implementation.
//  See seh_guard.h for the contract. Linux/macOS counterpart to seh_guard.cpp
//  (Windows VEH + RtlCaptureContext); this file exists because that mechanism
//  is Windows-only.
//
//  Mechanism: sigsetjmp/siglongjmp instead of VEH+RtlCaptureContext -- POSIX
//  has no equivalent of "restore full context and resume exactly where you
//  were", but seh_guarded_call() never actually needed that: on a fault it
//  always abandons the callback and reports failure, it never resumes INSIDE
//  the faulting code. That is exactly what siglongjmp out of the signal
//  handler back to the sigsetjmp() call site gives us, and it is the
//  standard, widely-used pattern for recovering from a SIGSEGV/SIGFPE/SIGILL
//  in a hosted/sandboxed call (used well beyond just plugin hosts). A
//  thread-local alternate signal stack (sigaltstack) is set up so the
//  handler itself has somewhere to run even when the fault IS a stack
//  overflow -- the case the guard is least allowed to fail on, since a
//  runaway/recursive bug in a plugin is a completely ordinary way for a
//  fault to happen.
//----------------------------------------------------------------------------
#include "seh_guard.h"

#include <xmmintrin.h>

#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace PatchKnob { namespace engine {

namespace {

struct GuardRec {
    sigjmp_buf env;
    GuardRec*  prev;
    volatile sig_atomic_t code;   // 0 = armed/no fault, else the signal number
    unsigned   mxcsr;             // captured MXCSR, restored after a fault
};

// The innermost armed guard on this thread (null = disarmed). The handler
// only acts when this is non-null, so an unguarded fault still crashes
// normally (SIG_DFL, core dump) instead of being silently swallowed.
thread_local GuardRec* t_guard = nullptr;

// The dispositions that were in place BEFORE we installed ours, so a fault we
// are not guarding can be handed back to whoever owned it. Plugins (and the
// runtimes some of them embed -- JITs, garbage collectors, JVM/mono bridges)
// legitimately install SIGSEGV/SIGBUS/SIGFPE handlers and depend on receiving
// those signals; installing over them process-wide and then answering every
// fault ourselves STEALS faults their owner was going to handle. We chain
// instead, and only fall back to SIG_DFL when there was no previous owner.
struct sigaction g_prevAction[NSIG];
bool             g_prevValid[NSIG];

bool chainToPrevious(int sig, siginfo_t* info, void* uctx)
{
    if (sig <= 0 || sig >= NSIG || !g_prevValid[sig]) return false;
    const struct sigaction& prev = g_prevAction[sig];

    // Call the previous owner DIRECTLY rather than swapping dispositions and
    // re-raising: it keeps our own handler installed (a swap would silently
    // uninstall us for the rest of the process) and hands the owner the exact
    // siginfo/ucontext it expects, which is what a fixup-and-return handler
    // (JIT guard page, GC write barrier, ...) needs to do its job.
    if ((prev.sa_flags & SA_SIGINFO) != 0)
    {
        if (prev.sa_sigaction == nullptr) return false;
        prev.sa_sigaction(sig, info, uctx);
        return true;
    }
    // SIG_IGN for a synchronous fault is undefined (returning would just
    // re-execute the faulting instruction forever), so treat it as "no owner".
    if (prev.sa_handler == SIG_DFL || prev.sa_handler == SIG_IGN) return false;
    prev.sa_handler(sig);
    return true;
}

void defaultAndReraise(int sig)
{
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
    raise(sig);
}

void faultHandler(int sig, siginfo_t* info, void* uctx)
{
    GuardRec* g = t_guard;
    if (!g || g->code != 0) {
        // Disarmed, or already mid-recovery: this fault is not ours to contain.
        // Give it back to whoever owned the signal before us; only if nobody
        // did do we fall through to the default disposition, so an unguarded
        // fault still terminates the process and dumps core as it normally
        // would.
        if (!chainToPrevious(sig, info, uctx))
            defaultAndReraise(sig);
        return;
    }
    g->code = sig;
    // siglongjmp is not on POSIX's strict async-signal-safe list, but
    // unwinding out of a fault handler this way is the standard, widely
    // deployed technique for exactly this problem (hosted/sandboxed calls
    // recovering from a crash) and is safe in practice on Linux/glibc: it
    // does no allocation and touches no locks, unlike printf or malloc.
    siglongjmp(g->env, sig);
}

// A dedicated stack for the handler to run on, so a fault that IS a stack
// overflow doesn't take the handler down with it. ~64KiB, well over
// MINSIGSTKSZ; lazily installed once per thread that ever arms a guard.
thread_local bool t_altStackReady = false;

void ensureAltStackForThisThread()
{
    if (t_altStackReady) return;
    static thread_local char altStack[65536];
    stack_t ss{};
    ss.ss_sp    = altStack;
    ss.ss_size  = sizeof(altStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);
    t_altStackReady = true;
}

// Hardware faults we contain, mirroring seh_guard.cpp's handled set as
// closely as POSIX signals allow. SIGABRT is deliberately NOT among them:
// unlike a hardware fault, abort() is code explicitly giving up, and
// swallowing that would hide real bugs (double-free, assert, etc.) instead
// of surfacing them.
void installHandlerOnce()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        struct sigaction sa{};
        sa.sa_sigaction = &faultHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        const int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL };
        for (int sig : sigs)
        {
            // Keep whatever was there so chainToPrevious() can give it back.
            g_prevValid[sig] = (sigaction(sig, &sa, &g_prevAction[sig]) == 0);
        }
    });
}

} // namespace

bool seh_guarded_call(void (*fn)(void*), void* arg, uint32_t* faultCode)
{
    installHandlerOnce();
    ensureAltStackForThisThread();

    GuardRec rec;
    rec.prev  = t_guard;
    rec.code  = 0;
    rec.mxcsr = _mm_getcsr();

    // sigsetjmp(..., 1): also save the signal mask, so siglongjmp restores
    // it -- without that the fault signal stays blocked (the OS auto-blocks
    // it for the handler's duration) after every recovery, and the SECOND
    // fault in a process would never be caught.
    if (sigsetjmp(rec.env, 1) != 0) {
        // Resumed here via siglongjmp from faultHandler(). rec.code is set;
        // sig_atomic_t plus the intervening signal delivery makes the
        // re-read here real, not something the optimizer can stale-cache.
        t_guard = rec.prev;
        __asm__ __volatile__("fninit");     // reset possibly-trashed x87 unit
        _mm_setcsr(rec.mxcsr);              // restore MXCSR from arm time
        if (faultCode) *faultCode = (uint32_t)rec.code;
        return false;
    }

    t_guard = &rec;                          // arm only after a clean setjmp
    fn(arg);
    t_guard = rec.prev;
    if (faultCode) *faultCode = 0;
    return true;
}

// ---------------------------------------------------------------------------
// self-test
// ---------------------------------------------------------------------------
namespace {

void faultNullWrite(void*)
{
    volatile int* p = nullptr;
    *p = 42;
}

void faultNullCall(void*)
{
    void (*volatile fp)() = nullptr;
    fp();
}

void faultDivZero(void* a)
{
    volatile int z = *(int*)a;              // 0, opaque to the optimizer
    volatile int r = 100 / z;
    (void)r;
}

void benign(void* a)
{
    *(int*)a = 1234;
}

void trashMxcsrThenFault(void*)
{
    _mm_setcsr(_mm_getcsr() | 0x8040);      // set FTZ|DAZ, then fault
    volatile int* p = nullptr;
    *p = 1;
}

void nestedOuter(void* a)
{
    // Inner guard catches; the outer guarded call must still succeed.
    uint32_t innerCode = 0;
    bool innerOk = seh_guarded_call(&faultNullWrite, nullptr, &innerCode);
    ((uint32_t*)a)[0] = innerOk ? 1u : 0u;
    ((uint32_t*)a)[1] = innerCode;
}

// A deep recursive blow-up -- exercises the sigaltstack path (the fault IS a
// stack overflow, so the handler must not need the faulting thread's own
// stack to run).
volatile int g_recDepth = 0;
void recurseUntilOverflow(void* a)
{
    char pad[4096]; std::memset(pad, (char)g_recDepth, sizeof(pad));
    ++g_recDepth;
    recurseUntilOverflow(a);
    (void)pad;
}

} // namespace

bool seh_guard_self_test()
{
    int fails = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) { std::fprintf(stderr, "seh_guard self-test FAIL: %s\n", name); ++fails; }
    };

    uint32_t code = 0;
    check(!seh_guarded_call(&faultNullWrite, nullptr, &code) &&
          code == (uint32_t)SIGSEGV, "null-write fault recovered");

    check(!seh_guarded_call(&faultNullCall, nullptr, &code) &&
          (code == (uint32_t)SIGSEGV || code == (uint32_t)SIGBUS),
          "null-function-pointer call recovered");

    int zero = 0;
    check(!seh_guarded_call(&faultDivZero, &zero, &code) &&
          code == (uint32_t)SIGFPE,
          "int divide-by-zero recovered");

    int out = 0;
    check(seh_guarded_call(&benign, &out, &code) && code == 0 && out == 1234,
          "benign call reports success");

    uint32_t nested[2] = { 99, 99 };
    check(seh_guarded_call(&nestedOuter, nested, &code) && code == 0 &&
          nested[0] == 0 && nested[1] == (uint32_t)SIGSEGV,
          "nested guard: inner fault caught, outer completes");

    bool repeatOk = true;
    for (int i = 0; i < 1000; ++i)
        if (seh_guarded_call(&faultNullWrite, nullptr, nullptr)) repeatOk = false;
    check(repeatOk, "1000 repeated faults all recovered");

    unsigned before = _mm_getcsr();
    seh_guarded_call(&trashMxcsrThenFault, nullptr, nullptr);
    check(_mm_getcsr() == before, "MXCSR restored after fault");

    g_recDepth = 0;
    check(!seh_guarded_call(&recurseUntilOverflow, nullptr, &code) &&
          code == (uint32_t)SIGSEGV,
          "stack overflow recovered (sigaltstack)");

    return fails == 0;
}

}} // namespace PatchKnob::engine
