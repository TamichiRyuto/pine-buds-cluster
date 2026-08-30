// Phase 0.5 bring-up: prove C++ runtime + FPU math on BES2300YP.
// Emits, in order, on the 2 Mbaud log UART:
//   [ctor] GlobalProbe constructed          <- static constructors ran
//   hello, C++ from PineBuds (core=N)       <- callable from app init
//   GEMM float N=32 checksum=... PASS/FAIL  <- FPU math self-verified
//
// Freestanding C++: no STL, no heap, no exceptions/RTTI, float only.

#include "compute_trace.h"
#include "gemm_selftest.h"
#include "mpi_ibrt_glue.h"

namespace {

// Static ctors run in __libc_init_array, BEFORE hal_trace_open(), so a
// TRACE from the ctor is lost (verified on device 2026-08-30). Record a
// magic value instead and report it later from compute_main(): .bss is
// zeroed before ctors, so the magic can only appear if the ctor ran.
const unsigned kCtorMagic = 0xC7B0BEEFu;
unsigned g_ctor_probe = 0u;

struct GlobalProbe {
    GlobalProbe() { g_ctor_probe = kCtorMagic; }
};
GlobalProbe global_probe;

unsigned current_core_id() {
    // Cortex-M: no MPIDR; on BES dual-M4F the boot/app core is core 0.
    // Revisit when the 2nd core is brought up (next phase).
    return 0u;
}

}  // namespace

extern "C" void compute_main(void) {
    if (g_ctor_probe == kCtorMagic) {
        COMPUTE_TRACE(0, "[ctor] GlobalProbe constructed");
    } else {
        COMPUTE_TRACE(1, "[ctor] MISSING: static ctor did not run (probe=0x%x)",
                      g_ctor_probe);
    }
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

    // MPI-over-IBRT bring-up (M-T1/M-T2/M-T3, docs/design-ibrt-transport.md
    // §9) continues asynchronously on its own compute thread; this call
    // returns immediately and does not block app_init.
    mpi_ibrt_glue_start();
}
