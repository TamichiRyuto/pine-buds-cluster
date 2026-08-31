// Thin trace/timing shim so compute code stays portable (gnu++98).
// On target it maps to the OpenPineBuds TRACE macro and hal timer;
// on the host it maps to printf.
//
// NOTE: the SDK's TRACE(attr, fmt, ...) takes the number of format
// arguments as its first parameter, so COMPUTE_TRACE does too.
//
// On target, every COMPUTE_TRACE line also reaches compute_log_tap (design
// docs/design-ibrt-transport.md §13.3.1/§13.3.3), which mirrors the same
// line over the SPP log channel (firmware/pinebuds_compute/spp_log_service.cpp).
// UART output (TRACE) is unchanged -- the tap is purely additive.
#ifndef COMPUTE_TRACE_H
#define COMPUTE_TRACE_H

#ifdef PINEBUDS_TARGET
#include "hal_timer.h"
#include "hal_trace.h"
void compute_log_tap(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define COMPUTE_TRACE(nargs, fmt, ...)       \
    do {                                     \
        TRACE(nargs, fmt, ##__VA_ARGS__);    \
        compute_log_tap(fmt, ##__VA_ARGS__); \
    } while (0)
static inline unsigned compute_tick_ms(void) {
    return (unsigned)GET_CURRENT_MS();
}
#else
#include <cstdio>
#define COMPUTE_TRACE(nargs, fmt, ...) std::printf(fmt "\n", ##__VA_ARGS__)
static inline unsigned compute_tick_ms(void) { return 0u; }
#endif

#endif  // COMPUTE_TRACE_H
