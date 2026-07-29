//----------------------------------------------------------------------------
//  PatchKnob (ZERO-JUCE) — SEH fault guard implementation.
//  See seh_guard.h for the contract.
//
//  Mechanism (chosen after probing both variants on this exact toolchain —
//  mingw64 GCC 15.2 — because __builtin_longjmp does not restore the Win64
//  callee-saved XMM6-15 registers a faulting plugin may have clobbered):
//    * arm:   RtlCaptureContext() snapshots the guard frame's full register
//             state into a thread-local GuardRec before calling the plugin.
//    * fault: the process-wide vectored exception handler (installed once)
//             sees an armed guard on the faulting thread, records the code,
//             copies the captured CONTEXT over the fault CONTEXT and returns
//             EXCEPTION_CONTINUE_EXECUTION — the kernel resumes execution
//             right after RtlCaptureContext with every non-volatile GPR/XMM
//             register exactly as it was at arm time.
//    * recover: the guard sees code != 0, disarms, resets the x87 unit and
//             restores MXCSR from the captured context (the aborted plugin
//             code may have left the FP environment trashed), and returns
//             false to the caller.
//----------------------------------------------------------------------------
#include "seh_guard.h"

#include <windows.h>
#include <xmmintrin.h>
#include <malloc.h>     // _resetstkoflw

#include <cstdio>
#include <mutex>

namespace PatchKnob { namespace engine {

namespace {

struct GuardRec {
    CONTEXT   ctx;              // non-volatile register state at arm time
    GuardRec* prev;             // enclosing guard (guards nest)
    volatile uint32_t code;     // 0 = armed/no fault, else NTSTATUS code
};

// The innermost armed guard on this thread (null = disarmed). The VEH only
// acts when this is non-null, so unguarded faults still crash normally.
thread_local GuardRec* t_guard = nullptr;

// Hardware faults we contain. C++ exceptions (0xE06D7363), breakpoints and
// anything else CONTINUE_SEARCH so debuggers and normal unwinding still work.
bool isHandledFault(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_STACK_OVERFLOW:
    case 0xC0000409u:   // STATUS_STACK_BUFFER_OVERRUN (fail-fast; best effort)
        return true;
    default:
        return false;
    }
}

LONG CALLBACK vehGuardHandler(EXCEPTION_POINTERS* xp)
{
    GuardRec* g = t_guard;
    if (!g || g->code != 0)     // disarmed, or already mid-recovery
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = xp->ExceptionRecord->ExceptionCode;
    if (!isHandledFault(code))
        return EXCEPTION_CONTINUE_SEARCH;
    g->code = code;
    // Restore the full non-volatile register state captured at arm time and
    // resume right after RtlCaptureContext; the guard sees code != 0 there.
    // (Volatile registers don't matter — they are dead at that resume point.)
    *xp->ContextRecord = g->ctx;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Install the VEH exactly once, process-wide. std::call_once's fast path is
// a single acquire load, so per-call overhead on the audio thread is nil.
void installHandlerOnce()
{
    static std::once_flag flag;
    std::call_once(flag, [] { AddVectoredExceptionHandler(1, &vehGuardHandler); });
}

} // namespace

bool seh_guarded_call(void (*fn)(void*), void* arg, uint32_t* faultCode)
{
    installHandlerOnce();

    GuardRec rec;
    rec.prev = t_guard;
    rec.code = 0;
    RtlCaptureContext(&rec.ctx);
    // Execution resumes HERE after a caught fault (context restored by VEH),
    // with rec.code holding the exception code. rec is address-taken (it
    // escapes into t_guard) and code is volatile, so the re-read is real.
    if (rec.code != 0)
    {
        t_guard = rec.prev;
        __asm__ __volatile__("fninit");     // reset possibly-trashed x87 unit
        _mm_setcsr(rec.ctx.MxCsr);          // restore MXCSR from arm time
        if (rec.code == (uint32_t)EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();                // re-arm the stack guard page
        if (faultCode) *faultCode = rec.code;
        return false;
    }
    t_guard = &rec;                          // arm only after a clean capture
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

} // namespace

bool seh_guard_self_test()
{
    int fails = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) { std::fprintf(stderr, "seh_guard self-test FAIL: %s\n", name); ++fails; }
    };

    uint32_t code = 0;
    check(!seh_guarded_call(&faultNullWrite, nullptr, &code) &&
          code == 0xC0000005u, "null-write fault recovered");

    check(!seh_guarded_call(&faultNullCall, nullptr, &code) &&
          code == 0xC0000005u, "null-function-pointer call recovered");

    int zero = 0;
    check(!seh_guarded_call(&faultDivZero, &zero, &code) &&
          code == (uint32_t)EXCEPTION_INT_DIVIDE_BY_ZERO,
          "int divide-by-zero recovered");

    int out = 0;
    check(seh_guarded_call(&benign, &out, &code) && code == 0 && out == 1234,
          "benign call reports success");

    uint32_t nested[2] = { 99, 99 };
    check(seh_guarded_call(&nestedOuter, nested, &code) && code == 0 &&
          nested[0] == 0 && nested[1] == 0xC0000005u,
          "nested guard: inner fault caught, outer completes");

    bool repeatOk = true;
    for (int i = 0; i < 1000; ++i)
        if (seh_guarded_call(&faultNullWrite, nullptr, nullptr)) repeatOk = false;
    check(repeatOk, "1000 repeated faults all recovered");

    unsigned before = _mm_getcsr();
    seh_guarded_call(&trashMxcsrThenFault, nullptr, nullptr);
    check(_mm_getcsr() == before, "MXCSR restored after fault");

    return fails == 0;
}

}} // namespace PatchKnob::engine
