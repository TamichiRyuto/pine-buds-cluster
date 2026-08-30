// Firmware glue between adapters/mpi and the IBRT TWS link.
// See docs/design-ibrt-transport.md §6 for the design and §9 for the
// M-T1/M-T2/M-T3 device milestones this file runs sequentially, in one
// boot, from a dedicated compute thread.
//
// Orchestrator deviations from the design doc:
//   - ONE cmdcode (0x8201) carries every MPI frame. DATA vs ACK is already
//     discriminated by mpi_frag's own frame-header kind byte (design §4),
//     so a second cmdcode buys nothing. PROBE/PROBE_ECHO reuse the same
//     cmdcode with kind values 3/4, handled entirely in this file -- they
//     are never fed to mpi_frag_on_frame.
//   - M-T1, M-T2, and M-T3 all run in this one boot, back to back, instead
//     of being separate flashed configurations.
//
// Target-only: this file is never compiled on host (it uses SDK headers
// freely), but stays gnu++98 (no C++11), heap-free, static-state-only,
// like the rest of firmware/pinebuds_compute/.

#include "mpi_ibrt_glue.h"

#include "compute_trace.h"
#include "mpi.h"
#include "mpi_adapter.h"
#include "mpi_frag.h"

#include <string.h>

// SDK headers. File:line references are to external/OpenPineBuds, cited in
// the task brief and docs/design-ibrt-transport.md §2/§6.
#include "app_tws_ctrl_thread.h"       // tws_ctrl_send_cmd
#include "app_tws_ibrt_cmd_handler.h"  // app_tws_cmd_instance_t, app_ibrt_set_cmdhandle, CMD_ID_T, app_ibrt_send_cmd_without_rsp
#include "app_tws_ibrt.h"              // app_tws_ibrt_tws_link_connected
#include "app_tws_if.h"                // app_tws_is_master_mode / app_tws_is_slave_mode
#include "cmsis_os.h"
#include "hal_timer.h"

// Design §10 risk 5: the TWS_CMD_OTA slot (cmdcode 0x82xx) is only free of
// services/bt_app/besmain.cpp's own registrations while none of these are
// defined (it registers TWS_CMD_IBRT_OTA under the first three, and
// TWS_CMD_OTA itself under __INTERACTION__). Fail the build loudly instead
// of silently colliding with the SDK's own cmd table.
#if defined(IBRT_OTA) || defined(__GMA_OTA_TWS__) || defined(BISTO_ENABLED) || \
    defined(__INTERACTION__)
#error "mpi_ibrt_glue: TWS_CMD_OTA slot (cmdcode 0x82xx) is claimed by IBRT_OTA / __GMA_OTA_TWS__ / BISTO_ENABLED / __INTERACTION__ in this build; pick a different cmdcode slot (docs/design-ibrt-transport.md §10 risk 5) before enabling any of them."
#endif

extern "C" int gemm_bench_run(int n, float *checksum_out);

namespace {

// ---------------------------------------------------------------------
// Wire constants.
const uint32_t kMpiIbrtCmdFrame = 0x8201u;

// kind byte values 3/4 share the wire with mpi_frag's own kind byte
// (MPI_FRAG_KIND_DATA=1, MPI_FRAG_KIND_ACK=2); PROBE/PROBE_ECHO frames are
// intercepted in the RX handler below and never reach mpi_frag_on_frame.
const unsigned char kKindProbe = 3;
const unsigned char kKindProbeEcho = 4;

typedef char kMpiIbrtKindCheck[(kKindProbe != MPI_FRAG_KIND_DATA &&
                                kKindProbe != MPI_FRAG_KIND_ACK &&
                                kKindProbeEcho != MPI_FRAG_KIND_DATA &&
                                kKindProbeEcho != MPI_FRAG_KIND_ACK)
                                   ? 1
                                   : -1];

const unsigned kLinkTimeoutMs = 10000u;
const unsigned kCreditTimeoutMs = 2000u;
const int kCreditWindow = 2;
// design §9: sweep must never exceed 672 (send-side assert in blob).
const int kProbeMaxLen = 668;

// ---------------------------------------------------------------------
// Glue state.
osMutexId g_port_mutex = 0;
osSemaphoreId g_wake_sem = 0;
osSemaphoreId g_credit_sem = 0;
osSemaphoreId g_probe_sem = 0;

osMutexDef(mpi_ibrt_port_mutex);
osSemaphoreDef(mpi_ibrt_wake_sem);
osSemaphoreDef(mpi_ibrt_credit_sem);
osSemaphoreDef(mpi_ibrt_probe_sem);

int g_self_rank = 0;
uint32_t g_wtime_base_ms = 0;

// Written on BesbtThread by the RX handler, read on the compute thread only
// after a successful osSemaphoreWait on g_probe_sem -- the semaphore
// provides the happens-before edge, volatile just prevents the compiler
// from caching the read.
volatile int g_probe_echo_len = -1;

// RX-side PROBE -> PROBE_ECHO reflect buffer. Single static instance is
// safe: tws_ctrl_send_cmd copies its buffer before returning (design
// §2.3), and only one PROBE is ever in flight (the peer waits for each
// echo before sending the next).
unsigned char g_probe_echo_tx[kProbeMaxLen];

// ---------------------------------------------------------------------
// RX handler -- runs on BesbtThread (design §2.5). Must never block: a
// PROBE gets an immediate echo (one non-blocking tws_ctrl_send_cmd), a
// PROBE_ECHO just records its length and releases a semaphore, and
// DATA/ACK hand off to mpi_frag (whose own ACK emit and deliver callback
// are equally non-blocking: a short mutex plus a memcpy).
void mpi_ibrt_cmdhandler(uint16_t rsp_seq, uint8_t *p_buff, uint16_t length) {
    (void)rsp_seq;
    if (length < 1) {
        return;
    }

    unsigned char kind = p_buff[0];

    if (kind == kKindProbe) {
        if (length > kProbeMaxLen) {
            return;  // malformed/oversized: drop rather than overrun the buffer
        }
        memcpy(g_probe_echo_tx, p_buff, length);
        g_probe_echo_tx[0] = kKindProbeEcho;
        tws_ctrl_send_cmd(kMpiIbrtCmdFrame, g_probe_echo_tx, length);
        return;
    }

    if (kind == kKindProbeEcho) {
        g_probe_echo_len = (int)length;
        osSemaphoreRelease(g_probe_sem);
        return;
    }

    mpi_frag_on_frame(p_buff, length);
}

// ---------------------------------------------------------------------
// Cmd table, modeled on
// services/app_ibrt/src/app_ibrt_customif_cmd.cpp:76-124. tws_cmd_send
// mirrors that file's pattern of calling app_ibrt_send_cmd_without_rsp; it
// is never invoked by the dispatcher (nothing in the SDK calls a table
// entry's tws_cmd_send generically) and is not on our hot path -- mpi_frag's
// emit and the probe code below call tws_ctrl_send_cmd directly so their
// link-down detection matches the verified §2.3 contract.
void mpi_ibrt_tws_cmd_send(uint8_t *p_buff, uint16_t length) {
    app_ibrt_send_cmd_without_rsp((uint16_t)kMpiIbrtCmdFrame, p_buff, length);
}

const app_tws_cmd_instance_t g_mpi_ibrt_cmd_table[] = {
    {kMpiIbrtCmdFrame, "MPI_IBRT_FRAME", mpi_ibrt_tws_cmd_send,
     mpi_ibrt_cmdhandler, 0, app_ibrt_cmd_rsp_timeout_handler_null,
     app_ibrt_cmd_rsp_handler_null},
};

int mpi_ibrt_cmd_table_get(void **cmd_tbl, uint16_t *cmd_size) {
    *cmd_tbl = (void *)&g_mpi_ibrt_cmd_table[0];
    *cmd_size = 1;
    return 0;
}

// ---------------------------------------------------------------------
// Port impl (design §6 item 3): condition-variable-style wait/wake over a
// counting semaphore, matching mpi_adapter_port's contract.
void mpi_ibrt_port_lock(void) { osMutexWait(g_port_mutex, osWaitForever); }
void mpi_ibrt_port_unlock(void) { osMutexRelease(g_port_mutex); }

void mpi_ibrt_port_wait(void) {
    // Unlock -> wait up to 100 ms -> relock. MPI_Recv/MPI_Wait re-check
    // their own predicate in a loop, so a spurious 100 ms timeout here is
    // harmless -- it just costs one extra iteration.
    mpi_ibrt_port_unlock();
    osSemaphoreWait(g_wake_sem, 100);
    mpi_ibrt_port_lock();
}

void mpi_ibrt_port_wake(void) { osSemaphoreRelease(g_wake_sem); }

int mpi_ibrt_port_self_rank(void) { return g_self_rank; }

// ---------------------------------------------------------------------
// Credits: a counting semaphore initialized with W tokens (design §5).
int mpi_ibrt_acquire_credit(void) {
    int32_t tokens = osSemaphoreWait(g_credit_sem, kCreditTimeoutMs);
    // cmsis_os v1: returns the tokens available on success (>=1), 0 on
    // timeout, -1 on error -- treat anything <= 0 as denied.
    return (tokens <= 0) ? 1 : 0;
}

void mpi_ibrt_release_credit(void) { osSemaphoreRelease(g_credit_sem); }

// ---------------------------------------------------------------------
// frag port: emit checks the link explicitly (tws_ctrl_send_cmd's own
// return value cannot distinguish success from link-down, design §2.3)
// then sends directly; deliver forwards straight to the adapter's RX seam.
int mpi_ibrt_frag_emit(const void *frame, int frame_len) {
    if (!app_tws_ibrt_tws_link_connected()) {
        return 1;
    }
    tws_ctrl_send_cmd(kMpiIbrtCmdFrame, (uint8_t *)frame, (uint16_t)frame_len);
    return 0;
}

int mpi_ibrt_transport_send(int src, int dest, int tag, const void *buf,
                            int byte_len) {
    return mpi_frag_send(src, dest, tag, buf, byte_len);
}

double mpi_ibrt_wtime(void) {
    // The target build uses -fsingle-precision-constant, which makes the
    // unsuffixed literal below parse as float; cast it to double explicitly
    // so the promotion is intentional rather than an implicit
    // -Wdouble-promotion warning (design §10 risk 8: MPI_Wtime is double
    // by API contract even though the FPU is single-precision-only).
    return (double)(uint32_t)(GET_CURRENT_MS() - g_wtime_base_ms) *
           (double)0.001;
}

// ---------------------------------------------------------------------
// TRACE's format engine has no float support (compute_main.cpp verified
// this on device); split into integer + 6-digit fraction, the same trick
// compute_main.cpp uses for its "%d.000000" checksum line, generalized for
// values that are not whole numbers.
void mpi_ibrt_float_to_parts(float v, int *whole, int *frac6) {
    int w = (int)v;
    float rem = v - (float)w;
    if (rem < 0.0f) {
        rem = -rem;
    }
    *whole = w;
    *frac6 = (int)(rem * 1000000.0f + 0.5f);
}

// ---------------------------------------------------------------------
// M-T1: custom-command round trip + payload sweep (design §9). Both ranks
// run this independently -- it never touches mpi_frag/mpi_adapter, only
// the raw PROBE/PROBE_ECHO kinds handled in the RX handler above.
void mpi_ibrt_run_probe_sweep() {
    static const int kLens[] = {4, 64, 128, 256, 268, 296, 328, 400, 512, 668};
    const int kLenCount = sizeof(kLens) / sizeof(kLens[0]);
    static unsigned char probe_buf[kProbeMaxLen];

    int max_ok = 0;

    for (int i = 0; i < kLenCount; ++i) {
        int len = kLens[i];
        for (int j = 0; j < len; ++j) {
            probe_buf[j] = (unsigned char)(j & 0xFF);
        }
        probe_buf[0] = kKindProbe;

        g_probe_echo_len = -1;
        unsigned t0 = compute_tick_ms();
        tws_ctrl_send_cmd(kMpiIbrtCmdFrame, probe_buf, (uint16_t)len);
        int32_t got = osSemaphoreWait(g_probe_sem, 500);
        unsigned t1 = compute_tick_ms();

        if (got > 0 && g_probe_echo_len == len) {
            COMPUTE_TRACE(2, "[mpi-t1] probe len=%d ok rtt=%d ms", len,
                          (int)(t1 - t0));
            max_ok = len;
        } else {
            COMPUTE_TRACE(1, "[mpi-t1] probe len=%d TIMEOUT", len);
        }
    }

    {
        const int kRttLen = 64;
        static unsigned char rtt_buf[kRttLen];
        for (int j = 0; j < kRttLen; ++j) {
            rtt_buf[j] = (unsigned char)(j & 0xFF);
        }
        rtt_buf[0] = kKindProbe;

        unsigned min_ms = 0xFFFFFFFFu;
        unsigned max_ms = 0u;
        unsigned sum_ms = 0u;
        int ok_count = 0;

        for (int iter = 0; iter < 100; ++iter) {
            g_probe_echo_len = -1;
            unsigned t0 = compute_tick_ms();
            tws_ctrl_send_cmd(kMpiIbrtCmdFrame, rtt_buf, (uint16_t)kRttLen);
            int32_t got = osSemaphoreWait(g_probe_sem, 500);
            unsigned t1 = compute_tick_ms();

            if (got > 0 && g_probe_echo_len == kRttLen) {
                unsigned rtt = t1 - t0;
                if (rtt < min_ms) {
                    min_ms = rtt;
                }
                if (rtt > max_ms) {
                    max_ms = rtt;
                }
                sum_ms += rtt;
                ++ok_count;
            }
        }

        unsigned avg_ms = (ok_count > 0) ? (sum_ms / (unsigned)ok_count) : 0u;
        if (ok_count == 0) {
            min_ms = 0u;
        }
        COMPUTE_TRACE(3, "[mpi-t1] rtt n=100 min=%u avg=%u max=%u ms", min_ms,
                      avg_ms, max_ms);
    }

    COMPUTE_TRACE(1, "[mpi-t1] max_payload=%d", max_ok);
}

// ---------------------------------------------------------------------
// M-T2: MPI-over-IBRT smoke test (design §9).
void mpi_ibrt_run_mt2(int rank) {
    MPI_Barrier(MPI_COMM_WORLD);
    COMPUTE_TRACE(0, "[mpi] barrier ok");

    if (rank == 0) {
        float val = 0.0f;
        MPI_Status status;
        MPI_Recv(&val, 1, MPI_FLOAT, 1, 7, MPI_COMM_WORLD, &status);
        int whole = 0;
        int frac6 = 0;
        mpi_ibrt_float_to_parts(val, &whole, &frac6);
        COMPUTE_TRACE(2, "[mpi] recv from=1 tag=7 val=%d.%06d", whole, frac6);
    } else {
        float val = 5.0f;
        MPI_Send(&val, 1, MPI_FLOAT, 0, 7, MPI_COMM_WORLD);
        COMPUTE_TRACE(0, "[mpi] send ok");
    }

    unsigned tx = 0;
    unsigned rx = 0;
    unsigned err = 0;
    mpi_frag_counters(&tx, &rx, &err);
    COMPUTE_TRACE(3, "[mpi] frames tx=%u rx=%u err=%u", tx, rx, err);
}

// ---------------------------------------------------------------------
// M-T3: unmodified bench (design §9). Rank 0 -- which is also the single
// bud in a degraded (size==1) run -- prints the checksum/timing lines;
// printing rank/size lets the log tell a real 2-bud run apart from a
// degraded single-bud one that also happens to pass.
void mpi_ibrt_run_mt3(int rank, int size) {
    const int kN = 32;
    const float kExpect = 32768.0f; /* 32^3 */
    float checksum = 0.0f;

    double t0 = MPI_Wtime();
    int rc = gemm_bench_run(kN, &checksum);
    double t1 = MPI_Wtime();

    unsigned elapsed_ms = (unsigned)((t1 - t0) * (double)1000.0);

    unsigned tx = 0;
    unsigned rx = 0;
    unsigned err = 0;
    mpi_frag_counters(&tx, &rx, &err);

    if (rank == 0) {
        int pass = (rc == 0) && (checksum == kExpect);
        COMPUTE_TRACE(6,
                      "GEMM-MPI N=%d rank=%d size=%d checksum=%d.000000 "
                      "expect=%d.000000 %s",
                      kN, rank, size, (int)checksum, (int)kExpect,
                      pass ? "PASS" : "FAIL");
        COMPUTE_TRACE(4, "GEMM-MPI elapsed=%u ms frames tx=%u rx=%u err=%u",
                      elapsed_ms, tx, rx, err);
    }
}

// ---------------------------------------------------------------------
// Compute thread body: link wait, bootstrap, then M-T1/M-T2/M-T3
// sequentially, all in this one boot (orchestrator deviation).
void mpi_compute_thread(void const *argument) {
    (void)argument;

    unsigned waited_ms = 0;
    int link_up = 0;
    while (waited_ms < kLinkTimeoutMs) {
        if (app_tws_ibrt_tws_link_connected()) {
            link_up = 1;
            break;
        }
        osDelay(50);
        waited_ms += 50;
    }

    int degraded = !link_up;
    int rank = 0;
    int size = 1;

    if (degraded) {
        COMPUTE_TRACE(1,
                      "[mpi] link not connected after %u ms; degraded "
                      "single-bud run",
                      waited_ms);
        mpi_adapter_bootstrap(0, 1);
    } else {
        rank = app_tws_is_master_mode() ? 0 : 1;
        size = 2;
        mpi_adapter_bootstrap(rank, size);
    }
    g_self_rank = rank;

    MPI_Init(0, 0);

    // Seams are installed unconditionally: harmless in degraded mode since
    // size==1 makes MPI_Barrier/MPI_Allreduce short-circuit before ever
    // touching the transport (mpi_core.cpp), and keeps this function's
    // control flow uniform between the two paths.
    static const mpi_adapter_port s_port = {
        &mpi_ibrt_port_self_rank, &mpi_ibrt_port_lock, &mpi_ibrt_port_unlock,
        &mpi_ibrt_port_wait, &mpi_ibrt_port_wake};
    mpi_adapter_set_port(&s_port);

    static const mpi_adapter_transport s_transport = {
        &mpi_ibrt_transport_send};
    mpi_adapter_set_transport(&s_transport);

    g_wtime_base_ms = (uint32_t)GET_CURRENT_MS();
    mpi_adapter_set_wtime(&mpi_ibrt_wtime);

    static const mpi_frag_port s_frag_port = {
        &mpi_ibrt_frag_emit, &mpi_adapter_deliver, &mpi_ibrt_acquire_credit,
        &mpi_ibrt_release_credit};
    mpi_frag_init(&s_frag_port);

    COMPUTE_TRACE(4, "[mpi] init rank=%d size=%d role=%s link_wait=%d ms",
                  rank, size,
                  degraded ? "NONE" : (rank == 0 ? "MASTER" : "SLAVE"),
                  (int)waited_ms);

    if (!degraded) {
        mpi_ibrt_run_probe_sweep();
        mpi_ibrt_run_mt2(rank);
    }

    mpi_ibrt_run_mt3(rank, size);

    MPI_Finalize();
    COMPUTE_TRACE(1, "[mpi] finalize done rank=%d", rank);
}

osThreadDef(mpi_compute_thread, osPriorityBelowNormal, 1, 4096, "mpi_compute");

}  // namespace

extern "C" void mpi_ibrt_glue_start(void) {
    g_port_mutex = osMutexCreate(osMutex(mpi_ibrt_port_mutex));
    g_wake_sem = osSemaphoreCreate(osSemaphore(mpi_ibrt_wake_sem), 0);
    g_credit_sem =
        osSemaphoreCreate(osSemaphore(mpi_ibrt_credit_sem), kCreditWindow);
    g_probe_sem = osSemaphoreCreate(osSemaphore(mpi_ibrt_probe_sem), 0);

    // TWS_CMD_OTA (index 2 of the cmdcode's top nibble) is unregistered in
    // this build (design §2.4/§10 risk 5; guarded above by the #error).
    app_ibrt_set_cmdhandle(TWS_CMD_OTA, mpi_ibrt_cmd_table_get);

    osThreadCreate(osThread(mpi_compute_thread), 0);
}
