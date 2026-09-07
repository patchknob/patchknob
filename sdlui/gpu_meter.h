//----------------------------------------------------------------------------
//  gpu_meter.h -- this process's GPU utilisation, for the menu-strip meter.
//
//  SDL exposes no such thing, so on Windows this reads the same performance
//  counters Task Manager does ("\GPU Engine(pid_N_...)\Utilization Percentage")
//  and sums the engines belonging to us.  Elsewhere it reports "unavailable".
//----------------------------------------------------------------------------
#ifndef PATCHKNOB_GPU_METER_H
#define PATCHKNOB_GPU_METER_H

namespace pkgpu {

//! 0..1 for this process, or a negative value when GPU load cannot be read.
//! Cheap to call every frame: it returns a cached sample and only re-queries
//! the (comparatively expensive) counters a couple of times a second.
float load();

//! Release the counter query.  Safe to call without a prior load().
void  shutdown();

} // namespace pkgpu

#endif
