//----------------------------------------------------------------------------
//  PatchKnob (ZERO-JUCE) — SEH fault guard for hosted plugin calls.
//
//  GCC/MinGW has no MSVC __try/__except, so a hardware fault raised inside a
//  third-party plugin (access violation, illegal instruction, integer divide
//  by zero, ...) would otherwise unwind unhandled and terminate the whole
//  app. seh_guarded_call() runs a callback under a vectored-exception-handler
//  guard: RtlCaptureContext snapshots the full non-volatile register state
//  before the call, and if the callback faults, the process-wide VEH restores
//  that context (so the caller's callee-saved GPR/XMM state is intact),
//  resumes inside the guard, restores the FP environment (the aborted plugin
//  code may leave MXCSR/x87 trashed), and reports failure so the host can
//  mark the plugin instance dead instead of crashing.
//
//  Threading: safe to call from any thread, including the realtime audio
//  thread — arming the guard is a context capture plus two thread-local
//  stores; no allocation, no locks (the vectored handler itself is installed
//  once, process-wide, on first use). Guards nest.
//
//  Verified on mingw64 GCC 15.2 / Windows 10 x64 at -O0 and -O2, single- and
//  multi-threaded, including callee-saved register survival across a fault
//  (see the seh_guard_self_test() cases run by vst2_test).
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_ENGINE_VST2_SEH_GUARD_H
#define PATCHKNOB_ENGINE_VST2_SEH_GUARD_H

#include <cstdint>

namespace PatchKnob { namespace engine {

// Run fn(arg) with the fault guard armed on this thread. Returns true if fn
// completed normally, false if a hardware fault (access violation, illegal
// instruction, int divide-by-zero, ... — see the handled set in
// seh_guard.cpp) occurred inside it. On fault, *faultCode (if non-null)
// receives the NTSTATUS exception code (e.g. 0xC0000005 access violation);
// on success it is set to 0.
bool seh_guarded_call(void (*fn)(void*), void* arg,
                      uint32_t* faultCode = nullptr);

// Self-test used by vst2_test: deliberately faults (null write, null
// function-pointer call, divide by zero, nested guards) inside guards and
// verifies recovery, fault codes, and MXCSR restoration. Returns true on
// pass; prints one line per failing case to stderr.
bool seh_guard_self_test();

}} // namespace PatchKnob::engine

#endif // PATCHKNOB_ENGINE_VST2_SEH_GUARD_H
