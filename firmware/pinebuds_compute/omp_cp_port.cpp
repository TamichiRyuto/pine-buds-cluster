// OpenMP worker port on the second Cortex-M4F via the SDK's cp_accel
// service (design doc docs/design-ibrt-transport.md §15). Mirrors the open
// sequence of apps/audioplayers/a2dp_decoder/a2dp_decoder_cp.c and uses the
// otherwise unused CP_TASK_HW slot.
//
// Everything the CP executes must live in .cp_text_sram (the CP's MPU denies
// flash, cp_accel.c:206): cp_main below, self_is_worker, and the runtime's
// omp_get_num_threads/omp_get_thread_num (adapters/omp, OMP_CP_TEXT). The
// port table itself must be in RAM too, not .rodata (flash), since the CP
// dereferences it through the runtime.

#include "omp_cp_port.h"

#include "compute_trace.h"
#include "omp_adapter.h"

#include "cmsis_os.h"
#include "cp_accel.h"
#include "hal_location.h"
#include "hal_timer.h"
#include "norflash_api.h"
#include "plat_addr_map.h"

namespace {

const uint8_t kEvtRun = 0;
const unsigned kOpenTimeoutMs = 200;
const unsigned kJoinTimeoutMs = 1000;

// Shared between the cores: plain SRAM, visible uncached to both.
struct WorkerJob {
    void (*fn)(void *);
    void *data;
    volatile int done;
};
WorkerJob g_job;

osSemaphoreId g_done_sem = 0;
osSemaphoreDef(omp_cp_done_sem);

int g_cp_open = 0;
unsigned g_worker_runs = 0;

// --- runs on the CP -----------------------------------------------------

CP_TEXT_SRAM_LOC int self_is_worker(void) {
    // No MPIDR on Cortex-M; tell the cores apart by the stack: the CP runs
    // on its MSP just below RAMCP_TOP (hal_cmu_cp_enable in cp_accel_open),
    // MCU thread stacks live in ordinary RAM below RAMCP_BASE.
    uint32_t sp;
    __asm volatile("mov %0, sp" : "=r"(sp));
    return (sp >= RAMCP_BASE && sp < RAMCP_BASE + RAMCP_SIZE) ? 1 : 0;
}

CP_TEXT_SRAM_LOC unsigned int cp_main(uint8_t event) {
    (void)event;
    if (g_job.fn) {
        g_worker_runs += (unsigned)self_is_worker();
        g_job.fn(g_job.data);
    }
    g_job.done = 1;
    cp_accel_send_event_cp2mcu(CP_BUILD_ID(CP_TASK_HW, kEvtRun));
    return 0;
}

// --- runs on the MCU ----------------------------------------------------

unsigned int mcu_evt_hdlr(uint8_t event) {
    (void)event;
    // mcu2cp IRQ context; osSemaphoreRelease is ISR-safe in CMSIS-RTOS v1.
    osSemaphoreRelease(g_done_sem);
    return 0;
}

int worker_count(void) { return g_cp_open ? 1 : 0; }

int worker_start(void (*fn)(void *), void *data) {
    if (!g_cp_open) {
        return -1;
    }
    g_job.fn = fn;
    g_job.data = data;
    g_job.done = 0;
    return cp_accel_send_event_mcu2cp(CP_BUILD_ID(CP_TASK_HW, kEvtRun));
}

void worker_join(void) {
    // The done flag is the truth; the semaphore only wakes us. A stale token
    // (from an earlier timeout) therefore cannot end a join early.
    uint32_t t0 = GET_CURRENT_MS();
    while (!g_job.done) {
        uint32_t waited = (uint32_t)(GET_CURRENT_MS() - t0);
        if (waited >= kJoinTimeoutMs) {
            COMPUTE_TRACE(1, "[omp] FAIL worker timeout after %u ms",
                          (unsigned)waited);
            break;
        }
        osSemaphoreWait(g_done_sem, kJoinTimeoutMs - waited);
    }
    g_job.fn = 0;
}

// In RAM (not const/.rodata) because the CP reads it via the runtime.
omp_port g_port = {&worker_count, &worker_start, &worker_join,
                   &self_is_worker};

// Statically initialised like TASK_DESC_A2DP; cp_accel copies the fields.
struct cp_task_desc g_desc = {CP_ACCEL_STATE_CLOSED, cp_main, 0, mcu_evt_hdlr,
                              0};

}  // namespace

extern "C" int omp_cp_port_init(void) {
    if (g_cp_open) {
        return 0;
    }
    if (!g_done_sem) {
        g_done_sem = osSemaphoreCreate(osSemaphore(omp_cp_done_sem), 0);
    }
    g_job.fn = 0;
    g_job.done = 1;

    // Same guard as a2dp_cp_init: keep flash idle while the CP boots and
    // copies .cp_text_sram/.cp_data_sram out of flash (system_cp_init).
    norflash_api_flush_disable(NORFLASH_API_USER_CP,
                               (uint32_t)cp_accel_init_done);
    int rc = cp_accel_open(CP_TASK_HW, &g_desc);
    unsigned waited_ms = 0;
    if (rc == 0) {
        while (!cp_accel_init_done() && waited_ms < kOpenTimeoutMs) {
            osDelay(1);
            ++waited_ms;
        }
    }
    norflash_api_flush_enable(NORFLASH_API_USER_CP);

    if (rc != 0 || !cp_accel_init_done()) {
        COMPUTE_TRACE(2, "[omp] FAIL cp open rc=%d init_done=%d", rc,
                      (int)cp_accel_init_done());
        cp_accel_close(CP_TASK_HW);
        return -1;
    }
    g_cp_open = 1;
    omp_set_port(&g_port);
    COMPUTE_TRACE(1, "[omp] cp open ok after %u ms", waited_ms);
    return 0;
}

extern "C" unsigned omp_cp_port_worker_runs(void) { return g_worker_runs; }
