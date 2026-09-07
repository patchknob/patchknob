#pragma once

#ifdef __SSE4_2__
	#include <nmmintrin.h>
#else
	#ifndef SIMDE_ENABLE_NATIVE_ALIASES
		#define SIMDE_ENABLE_NATIVE_ALIASES
	#endif
	#include <simde/x86/sse4.2.h>
	/* SIMDe 0.7 aliases the rounding modes on ARM except NO_EXC. */
	#ifndef _MM_FROUND_NO_EXC
		#define _MM_FROUND_NO_EXC SIMDE_MM_FROUND_NO_EXC
	#endif
#endif
