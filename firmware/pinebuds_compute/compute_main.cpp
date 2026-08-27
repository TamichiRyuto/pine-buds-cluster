// Phase 0.5 bring-up: prove C++ runtime + FPU math on BES2300YP.
// Emits, in order, on the 2 Mbaud log UART:
//   [ctor] GlobalProbe constructed          <- static constructors ran
//   hello, C++ from PineBuds (core=N)       <- callable from app init
//   GEMM float N=32 checksum=... PASS/FAIL  <- FPU math self-verified
//
// Freestanding C++: no STL, no heap, no exceptions/RTTI, float only.

#include "compute_trace.h"
#include "gemm_selftest.h"

namespace {

// Its constructor writing a trace line proves __libc_init_array ran.
struct GlobalProbe {
    GlobalProbe() { COMPUTE_TRACE(0, "[ctor] GlobalProbe constructed"); }
};
GlobalProbe global_probe;

unsigned current_core_id() {
    // Cortex-M: no MPIDR; on BES dual-M4F the boot/app core is core 0.
    // Revisit when the 2nd core is brought up (next phase).
    return 0u;
}

}  // namespace

extern "C" void compute_main(void) {
    COMPUTE_TRACE(1, "hello, C++ from PineBuds (core=%u)", current_core_id());

    const unsigned t0 = compute_tick_ms();
    const GemmSelftestResult r = gemm_selftest();
    const unsigned t1 = compute_tick_ms();

    // %f may be unsupported by the firmware's printf; integers are exact
    // here anyway (checksum is integer-valued by construction).
    COMPUTE_TRACE(4, "GEMM float N=%d  checksum=%d.000000  expect=%d.000000  %s",
                  kGemmSelftestN, (int)r.checksum, (int)r.expect,
                  r.pass ? "PASS" : "FAIL");
    if (!r.pass) {
        COMPUTE_TRACE(3, "GEMM first mismatch at (%d,%d) value=%d", r.fail_i,
                      r.fail_j, (int)r.fail_value);
    }
    COMPUTE_TRACE(1, "GEMM elapsed=%u ms", t1 - t0);
}
