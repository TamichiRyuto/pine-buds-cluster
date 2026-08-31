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
// the task brief and docs/design-ibrt-transport.md §2/§6/§11.
#include "app_tws_ctrl_thread.h"       // tws_ctrl_send_cmd
#include "app_tws_ibrt_cmd_handler.h"  // app_tws_cmd_instance_t, app_ibrt_set_cmdhandle, CMD_ID_T, app_ibrt_send_cmd_without_rsp
#include "app_tws_ibrt.h"              // ibrt_ctrl_t, app_tws_ibrt_get_bt_ctrl_ctx, app_tws_ibrt_tws_link_connected, app_tws_ibrt_create_tws_connection
#include "app_tws_if.h"                // app_tws_is_right_side / app_tws_is_unknown_side
#include "app_ibrt_ui.h"               // app_ibrt_ui_t, app_ibrt_ui_get_ctx, IBRT_OUT_BOX, IBRT_OPEN_BOX_EVENT, IBRT_FETCH_OUT_EVENT
#include "app_ibrt_if.h"               // app_ibrt_if_event_entry
#include "besaud_api.h"                // btif_besaud_is_connected
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

const unsigned kCreditTimeoutMs = 2000u;
const int kCreditWindow = 2;
// design §9: sweep must never exceed 672 (send-side assert in blob).
const int kProbeMaxLen = 668;

// design §11.2.3 bring-up sequence timing.
const unsigned kStackReadyPollMs = 50u;
const unsigned kStackReadyTimeoutMs = 15000u;
const unsigned kBoxForceDelayMs = 200u;
const unsigned kCmdChannelPollMs = 50u;
const unsigned kCmdChannelTimeoutMs = 20000u;      // raised from 10000 (§11.2.3 step 4)
const unsigned kCmdChannelFallbackMs = 8000u;      // fallback (b) trigger point
// design §11.2.5 item 4/5 guard timings.
const unsigned kPeerEchoTimeoutMs = 2000u;
const unsigned kStallWarnMs = 5000u;

// design §11.4 risk 1: pin the DWARF-verified struct sizes so a future
// SDK/lib swap that reflows ibrt_ctrl_t / app_ibrt_ui_t's field offsets
// fails the build instead of silently corrupting state through the direct
// pointer reads/writes below.
typedef char kMpiIbrtStructSizeCheck[(sizeof(ibrt_ctrl_t) == 280 &&
                                      sizeof(app_ibrt_ui_t) == 804)
                                         ? 1
                                         : -1];

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

// design §11.2.5 item 4: byte[1] of a PROBE_ECHO carries the responder's
// own rank (written by the RX handler below before it echoes). Same
// happens-before edge as g_probe_echo_len.
volatile int g_probe_echo_rank = -1;

// RX-side PROBE -> PROBE_ECHO reflect buffer. Single static instance is
// safe: tws_ctrl_send_cmd copies its buffer before returning (design
// §2.3), and only one PROBE is ever in flight (the peer waits for each
// echo before sending the next).
unsigned char g_probe_echo_tx[kProbeMaxLen];

// ---------------------------------------------------------------------
// design §11.2.1: two-stage readiness. Stage 1 -- has the SDK's own
// app_ibrt_init() (apps.cpp:1845-1866) actually run? .bss is zeroed at
// reset, so nv_role/current_role/tws_conhandle all read as valid-looking
// zero/garbage before this; init_done/is_ibrt_search_ui/bonding_success
// are the only fields that gate correctly (§11.1 (1),(3)).
int mpi_ibrt_stack_ready(void) {
    ibrt_ctrl_t *c = app_tws_ibrt_get_bt_ctrl_ctx();
    app_ibrt_ui_t *u = app_ibrt_ui_get_ctx();
    return c->init_done            /* app_tws_ibrt_init() tail,  apps.cpp:1849 */
        && c->is_ibrt_search_ui    /* app_tws_ibrt_start(cfg,true), apps.cpp:1855 */
        && u->bonding_success;     /* app_ibrt_ui_init(),        apps.cpp:1850 */
}

// Stage 2 -- does 0x8201 actually reach the wire? Same predicate the blob
// itself gates tws_ctrl_send_cmd's payload heap on (§2.3); this is also
// mpi_ibrt_frag_emit's send gate, so any glue-triggered readiness change
// here directly matches the framing layer's own idea of "connected".
int mpi_ibrt_cmd_channel_ready(void) {
    return mpi_ibrt_stack_ready()
        && app_tws_ibrt_tws_link_connected()  /* tws_conhandle != 0xFFFF */
        && btif_besaud_is_connected();        /* besaud_api.h:28 */
}

// ---------------------------------------------------------------------
// RX handler -- runs on BesbtThread (design §2.5). Must never block: a
// PROBE gets an immediate echo (one non-blocking tws_ctrl_send_cmd), a
// PROBE_ECHO just records its length/rank and releases a semaphore, and
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
        if (length >= 2) {
            // design §11.2.5 item 4: stamp our own rank into byte[1] so the
            // sender can read back who actually answered, instead of its
            // own byte[1] merely bouncing unmodified.
            g_probe_echo_tx[1] = (unsigned char)g_self_rank;
        }
        tws_ctrl_send_cmd(kMpiIbrtCmdFrame, g_probe_echo_tx, length);
        return;
    }

    if (kind == kKindProbeEcho) {
        g_probe_echo_len = (int)length;
        g_probe_echo_rank = (length >= 2) ? (int)p_buff[1] : -1;
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

// design §11.2.5 item 5: watchdog state for mpi_ibrt_port_wait. The MPI API
// contract gives us no way to abort a blocked Send/Recv/Wait, so this is
// detection-only -- one FAIL line, then keep waiting.
unsigned g_wait_timeout_streak_ms = 0;
int g_stall_warned = 0;

void mpi_ibrt_port_wait(void) {
    // Unlock -> wait up to 100 ms -> relock. MPI_Recv/MPI_Wait re-check
    // their own predicate in a loop, so a spurious 100 ms timeout here is
    // harmless -- it just costs one extra iteration.
    mpi_ibrt_port_unlock();
    int32_t got = osSemaphoreWait(g_wake_sem, 100);
    mpi_ibrt_port_lock();

    if (got > 0) {
        // Real (or spurious) wake: the stall, if any, is over. Re-arm the
        // one-shot warning for the next stall.
        g_wait_timeout_streak_ms = 0;
        g_stall_warned = 0;
        return;
    }

    g_wait_timeout_streak_ms += 100u;
    if (!g_stall_warned && g_wait_timeout_streak_ms > kStallWarnMs) {
        g_stall_warned = 1;
        unsigned tx = 0;
        unsigned rx = 0;
        unsigned err = 0;
        mpi_frag_counters(&tx, &rx, &err);
        COMPUTE_TRACE(5,
                      "[mpi] FAIL stalled in MPI op >%u ms rank=%d tx=%u "
                      "rx=%u err=%u",
                      kStallWarnMs, g_self_rank, tx, rx, err);
    }
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
// frag port: emit checks the full cmd-channel predicate explicitly
// (tws_ctrl_send_cmd's own return value cannot distinguish success from
// link-down, design §2.3; app_tws_ibrt_tws_link_connected() alone is not
// enough either -- design §11.1 (1)/§11.2.1) then sends directly; deliver
// forwards straight to the adapter's RX seam.
int mpi_ibrt_frag_emit(const void *frame, int frame_len) {
    if (!mpi_ibrt_cmd_channel_ready()) {
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
// design §11.2.5 item 4: rank handshake. Reuses the PROBE/PROBE_ECHO wire
// M-T1 uses below -- the RX handler stamps its own rank into byte[1] of
// every echo (see mpi_ibrt_cmdhandler), so this is just one PROBE send +
// wait, reading the echoed byte[1] back as the peer's rank. Runs once,
// after the cmd channel is up and before M-T1/M-T2, to catch the (2)/(4)
// failure modes from §11.1 (both-MASTER rank collision, no reachable
// peer) before any MPI collective can block forever on them.
int mpi_ibrt_run_rank_handshake(int self_rank) {
    const int kRankProbeLen = 4;
    static unsigned char probe[kRankProbeLen];
    probe[0] = kKindProbe;
    probe[1] = (unsigned char)self_rank;
    probe[2] = 0;
    probe[3] = 0;

    g_probe_echo_len = -1;
    g_probe_echo_rank = -1;
    tws_ctrl_send_cmd(kMpiIbrtCmdFrame, probe, (uint16_t)kRankProbeLen);
    int32_t got = osSemaphoreWait(g_probe_sem, kPeerEchoTimeoutMs);

    if (got <= 0) {
        COMPUTE_TRACE(0, "[mpi] FAIL no peer echo");
        return -1;
    }

    int peer_rank = g_probe_echo_rank;
    if (peer_rank == self_rank) {
        COMPUTE_TRACE(2, "[mpi] FAIL rank collision self=%d peer=%d",
                      self_rank, peer_rank);
        return -1;
    }

    COMPUTE_TRACE(2, "[mpi] peer ok rank=%d peer=%d", self_rank, peer_rank);
    return peer_rank;
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
// Seam install (design §6 items 3/4/6/7), unchanged in content from the
// pre-§11 version -- just factored out so both the normal and degraded
// tails below can share it without duplicating five lines three times.
// Harmless to call more than once (mpi_adapter_bootstrap/MPI_Init reset
// only rank/size/init flags, mpi_core.cpp), and harmless in degraded mode
// since size==1 makes MPI_Barrier/MPI_Allreduce short-circuit before ever
// touching the transport.
void mpi_ibrt_install_seams(int rank, int size) {
    mpi_adapter_bootstrap(rank, size);
    g_self_rank = rank;
    MPI_Init(0, 0);

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
}

// ---------------------------------------------------------------------
// Compute thread body: bring-up guards (design §11.2.3/§11.2.5), then
// M-T1/M-T2/M-T3 sequentially, all in this one boot (orchestrator
// deviation). Every exit prints exactly one line before falling through
// to the shared degraded tail -- no silent hang (design §11.2.5).
void mpi_compute_thread(void const *argument) {
    (void)argument;

    ibrt_ctrl_t *ctrl = app_tws_ibrt_get_bt_ctrl_ctx();
    app_ibrt_ui_t *ui = app_ibrt_ui_get_ctx();

    int degraded = 0;
    int rank = 0;
    unsigned link_wait_ms = 0;

    // §11.2.2: rank comes from the L/R strap, latched before BT bring-up
    // (app_tws_set_side_from_gpio() at app_init entry, apps.cpp:1926) --
    // never from current_role/nv_role, which read as 0/MASTER for both
    // buds while .bss is still zeroed (§11.1 (2)).
    if (app_tws_is_unknown_side()) {
        COMPUTE_TRACE(0,
                      "[mpi] FAIL side strap unknown; degraded single-bud "
                      "run");
        degraded = 1;
    } else {
        rank = app_tws_is_right_side() ? 0 : 1;
    }

    // §11.2.3 step 1: wait for stage-1 (stack) readiness.
    if (!degraded) {
        unsigned waited_ms = 0;
        int stack_ready = 0;
        while (waited_ms < kStackReadyTimeoutMs) {
            if (mpi_ibrt_stack_ready()) {
                stack_ready = 1;
                break;
            }
            osDelay(kStackReadyPollMs);
            waited_ms += kStackReadyPollMs;
        }

        // design §11.3 line 1 -- printed as soon as ctrl/ui fields are
        // meaningful, before anything else can fail.
        COMPUTE_TRACE(5,
                      "[mpi] side=%s rank=%d nv_role=%s current_role=%s "
                      "init_done=%d",
                      (rank == 0) ? "RIGHT" : "LEFT", rank,
                      app_tws_ibrt_role2str(ctrl->nv_role),
                      app_tws_ibrt_role2str(ctrl->current_role),
                      (int)ctrl->init_done);

        if (!stack_ready) {
            COMPUTE_TRACE(4,
                          "[mpi] FAIL stack not ready after %u ms "
                          "(init_done=%d search_ui=%d bonding=%d)",
                          waited_ms, (int)ctrl->init_done,
                          (int)ctrl->is_ibrt_search_ui,
                          (int)ui->bonding_success);
            degraded = 1;
        } else if (ctrl->nv_role == IBRT_UNKNOW) {
            // §11.2.3 step 2: unpaired -- TWS needs inquiry, which cannot
            // happen in-case. No event injection; degrade and let the
            // manual first-pairing procedure (docs) handle it once.
            COMPUTE_TRACE(0,
                          "[mpi] FAIL nv_role unknown; pair once outside "
                          "the case first");
            degraded = 1;
        }
    }

    int size = 1;

    // §11.2.3 steps 3-4: force the SDK's own out-of-case bring-up entry
    // point, then wait for stage-2 (cmd channel) readiness, with the
    // MASTER-only create_tws_connection() kick as fallback (b).
    if (!degraded) {
        ui->box_state = IBRT_OUT_BOX;  // precedent: app_ibrt_search_pair_ui.cpp:669,691
        app_ibrt_if_event_entry(IBRT_OPEN_BOX_EVENT);
        osDelay(kBoxForceDelayMs);
        app_ibrt_if_event_entry(IBRT_FETCH_OUT_EVENT);
        COMPUTE_TRACE(0,
                      "[mpi] box forced OUT_BOX, injecting OPEN_BOX + "
                      "FETCH_OUT");

        int cmd_ready = 0;
        int fallback_fired = 0;
        while (link_wait_ms < kCmdChannelTimeoutMs) {
            if (mpi_ibrt_cmd_channel_ready()) {
                cmd_ready = 1;
                break;
            }
            if (!fallback_fired && link_wait_ms >= kCmdChannelFallbackMs &&
                ctrl->nv_role == IBRT_MASTER) {
                // fallback (b): slave-side call is a no-op by design
                // (§11.2.3), so only MASTER ever calls this, and only once.
                fallback_fired = 1;
                app_tws_ibrt_create_tws_connection(
                    ctrl->config.tws_connection_timeout);
            }
            osDelay(kCmdChannelPollMs);
            link_wait_ms += kCmdChannelPollMs;
        }

        if (!cmd_ready) {
            COMPUTE_TRACE(3,
                          "[mpi] FAIL cmd channel down after %u ms "
                          "(link=%d besaud=%d)",
                          link_wait_ms,
                          (int)app_tws_ibrt_tws_link_connected(),
                          (int)btif_besaud_is_connected());
            degraded = 1;
        } else {
            size = 2;
        }
    }

    int self_rank = degraded ? 0 : rank;

    mpi_ibrt_install_seams(self_rank, size);

    COMPUTE_TRACE(4, "[mpi] init rank=%d size=%d link_wait=%u ms besaud=%d",
                  self_rank, size, link_wait_ms,
                  (int)btif_besaud_is_connected());

    // §11.2.5 item 4: rank handshake -- only meaningful once the cmd
    // channel actually holds. A collision or missing peer here means the
    // link that mpi_ibrt_cmd_channel_ready() reported as up cannot
    // actually reach a distinct MPI peer, so fall back to the same
    // degraded (size==1) tail as any other bring-up failure.
    if (!degraded) {
        int peer_rank = mpi_ibrt_run_rank_handshake(self_rank);
        if (peer_rank < 0) {
            degraded = 1;
            self_rank = 0;
            size = 1;
            mpi_ibrt_install_seams(self_rank, size);
        }
    }

    if (!degraded) {
        mpi_ibrt_run_probe_sweep();
        mpi_ibrt_run_mt2(self_rank);
    }

    mpi_ibrt_run_mt3(self_rank, size);

    MPI_Finalize();
    COMPUTE_TRACE(1, "[mpi] finalize done rank=%d", self_rank);

    // Never return: with __RTX_CPU_STATISTICS__=1 this SDK's RTX faults on
    // thread self-termination (rt_tsk_delete NULLs os_tsk.run, then
    // rt_switch_req dereferences it; design doc §11.5). Park like every
    // other SDK thread does.
    for (;;) {
        osDelay(10000);
    }
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
