// Thin trace/timing shim so compute code stays portable (gnu++98).
// On target it maps to the OpenPineBuds TRACE macro and hal timer;
// on the host it maps to printf.
//
// NOTE: the SDK's TRACE(attr, fmt, ...) takes the number of format
// arguments as its first parameter, so COMPUTE_TRACE does too.
#ifndef COMPUTE_TRACE_H
#define COMPUTE_TRACE_H

#ifdef PINEBUDS_TARGET
#include "hal_timer.h"
#include "hal_trace.h"
#define COMPUTE_TRACE(nargs, fmt, ...) TRACE(nargs, fmt, ##__VA_ARGS__)
static inline unsigned compute_tick_ms(void) {
    return (unsigned)GET_CURRENT_MS();
}
#else
#include <cstdio>
#define COMPUTE_TRACE(nargs, fmt, ...) std::printf(fmt "\n", ##__VA_ARGS__)
static inline unsigned compute_tick_ms(void) { return 0u; }
#endif

#endif  // COMPUTE_TRACE_H
