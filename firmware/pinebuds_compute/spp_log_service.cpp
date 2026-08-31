// SDK-facing SPP log channel (docs/design-ibrt-transport.md §13).
//
// Streams every line that goes through COMPUTE_TRACE (§13.3.1) to a
// Windows PC over Bluetooth SPP, in parallel with the unmodified 2 Mbaud
// UART output. Three independent pieces, per the design:
//   - §13.2  SDP record (SerialPort 0x1101) + RFCOMM server (channel 19,
//            the sole unused RFCOMM_CHANNEL_10/BTIF_APP_SPP_SERVER_ID_10
//            slot in this build, §13.1.2).
//   - §13.3  The tap (compute_log_tap, called from COMPUTE_TRACE) and the
//            send thread that drains firmware/pinebuds_compute/log_ring
//            over btif_spp_write, driven by DATA_SENT.
//   - §13.4  Access-mode re-arm: once MPI hands off (mpi_ibrt_glue.cpp,
//            "[mpi] peer ok"), periodically re-assert
//            BTIF_BAM_CONNECTABLE_ONLY so a PC can reconnect without
//            touching the in-case TWS bring-up sequence (§13.4.3 case C).
//
// §13.1.5: uses btif_spp_*/btif_sdp_* directly, never app_spp_send_data()
// (leaks umm_malloc, §13.1.1-3) or app_rfcomm_mgr.cpp (hardcodes a 128-bit
// UUID, §13.1.5).
//
// Target-only: this file is never compiled on host (it uses SDK headers
// freely), but stays gnu++98 (no C++11), heap-free, static-state-only,
// like the rest of firmware/pinebuds_compute/ (see mpi_ibrt_glue.cpp).

#include "spp_log_service.h"

#include "compute_trace.h"  // TRACE (via hal_trace.h, pulled in under PINEBUDS_TARGET)
#include "log_ring.h"

#include <stdarg.h>
#include <stdio.h>

// SDK headers. File:line references are to external/OpenPineBuds, verified
// against docs/design-ibrt-transport.md §13.1-§13.4 during implementation.
#include "spp_api.h"       // struct spp_device/spp_service, btif_spp_*, RFCOMM_CHANNEL_10, BTIF_SPP_*
#include "bt_if.h"         // BTIF_APP_SPP_SERVER_ID_10
#include "app_tws_ibrt.h"  // ibrt_ctrl_t, app_tws_ibrt_get_bt_ctrl_ctx, app_tws_ibrt_set_access_mode
#include "me_api.h"        // BTIF_BAM_CONNECTABLE_ONLY, BTIF_COD_MAJOR_PERIPHERAL
#include "cmsis_os.h"

namespace {

// ---------------------------------------------------------------------
// §13.2.2: SDP record. Byte-for-byte the same shape as
// services/tota/app_spp_tota.cpp:157-210's TotaSppClassId/...ServiceName1,
// just with the standard SerialPort UUID kept (never replaced, §13.2.1)
// and no custom 128-bit UUID added.
const U8 kLogSppClassId[] = {
    SDP_ATTRIB_HEADER_8BIT(3),
    SDP_UUID_16BIT(SC_SERIAL_PORT), /* 0x1101 */
};

const U8 kLogSppProtoDescList[] = {
    SDP_ATTRIB_HEADER_8BIT(12),
    SDP_ATTRIB_HEADER_8BIT(3),
    SDP_UUID_16BIT(PROT_L2CAP),
    SDP_ATTRIB_HEADER_8BIT(5),
    SDP_UUID_16BIT(PROT_RFCOMM),
    SDP_UINT_8BIT(RFCOMM_CHANNEL_10), /* = 19, sole unused slot, §13.1.2 */
};

const U8 kLogSppProfileDescList[] = {
    SDP_ATTRIB_HEADER_8BIT(8),
    SDP_ATTRIB_HEADER_8BIT(6),
    SDP_UUID_16BIT(SC_SERIAL_PORT),
    SDP_UINT_16BIT(0x0102), /* SPP v1.2 */
};

const U8 kLogSppServiceName[] = {
    SDP_TEXT_8BIT(12), 'P', 'i', 'n', 'e', 'B', 'u', 'd', 's', 'L',
    'o',               'g', '\0'};

sdp_attribute_t g_log_spp_sdp_attributes[] = {
    SDP_ATTRIBUTE(AID_SERVICE_CLASS_ID_LIST, kLogSppClassId),
    SDP_ATTRIBUTE(AID_PROTOCOL_DESC_LIST, kLogSppProtoDescList),
    SDP_ATTRIBUTE(AID_BT_PROFILE_DESC_LIST, kLogSppProfileDescList),
    SDP_ATTRIBUTE((AID_SERVICE_NAME + 0x0100), kLogSppServiceName),
};

// ---------------------------------------------------------------------
// §13.2.2 device/service state, plus the ring + thread wiring the design's
// excerpt leaves implicit (log_ring_init/osMutexCreate/osThreadCreate).
struct spp_device *s_dev;
struct spp_service *s_service;
btif_sdp_record_t *s_record;
uint8_t s_rx_buf[256]; /* unused for RX; btif_spp_init_rx_buf still requires one */

osMutexDef(spp_log_mutex);
osMutexDef(spp_log_credit_mutex);
osMutexDef(spp_log_ring_mutex);

enum { kSppLogTxSlots = 2 }; /* btif_spp_init_device numPackets, §13.3.4 */

// §13.3.3 tap state.
struct log_ring s_ring;
osMutexId s_ring_mid;
osThreadId s_log_tid;
unsigned s_seq;
unsigned s_contended; /* try-lock failures, statistics only */

// §13.3.4 send state machine.
enum { kSppLogChunk = 512 }; /* < L2CAP_MTU 672, app_spp.h:29 */
char s_tx_slot[kSppLogChunk];
volatile int s_connected;
volatile int s_inflight;
volatile unsigned s_done_len; /* written on BesbtThread, read on log thread */
unsigned s_inflight_base;
unsigned s_inflight_len;

// §13.4.3 access-mode re-arm state.
int s_scan_forced;

// ---------------------------------------------------------------------
// §13.2.2: RX is unused; register a discard handler explicitly rather than
// leaving spp_handle_data_event_func NULL (spp_read_thread already treats
// NULL as a no-op, but this documents the intent).
int spp_log_rx_discard(void *dev, uint8_t process, uint8_t *data,
                       uint16_t len) {
    (void)dev;
    (void)process;
    (void)data;
    (void)len;
    return 0;
}

// ---------------------------------------------------------------------
// §13.4.3: called from the log thread every 200 ms. Only re-asserts the
// access mode when it has drifted (the SDK's own duplicate-set filter would
// no-op the HCI command anyway, but skipping app_tws_ibrt_set_access_mode
// entirely avoids depending on that filter existing at all call sites).
void spp_log_rearm_access_mode(void) {
    if (!s_scan_forced) {
        return;
    }
    ibrt_ctrl_t *c = app_tws_ibrt_get_bt_ctrl_ctx();
    if (c->access_mode_sending) {
        return;
    }
    if (c->access_mode != BTIF_BAM_CONNECTABLE_ONLY) {
        app_tws_ibrt_set_access_mode(BTIF_BAM_CONNECTABLE_ONLY);
    }
}

// ---------------------------------------------------------------------
// §13.1.3/§13.3.4: runs on BesbtThread. Must never block or touch the ring
// -- only flag state, then hand the real work to the log thread.
void spp_log_callback(struct spp_device *dev, struct spp_callback_parms *info) {
    (void)dev;
    switch (info->event) {
    case BTIF_SPP_EVENT_REMDEV_CONNECTED:
        s_connected = 1;
        s_inflight = 0;
        TRACE(0, "[spplog] connected");
        break;
    case BTIF_SPP_EVENT_REMDEV_DISCONNECTED:
        s_connected = 0;
        s_inflight = 0;
        TRACE(0, "[spplog] disconnected");
        break;
    case BTIF_SPP_EVENT_DATA_SENT: {
        struct spp_tx_done *d = (struct spp_tx_done *)info->p.other;
        s_done_len = d->tx_data_length;
        s_inflight = 0;
        break;
    }
    default:
        break;
    }
    osSignalSet(s_log_tid, 0x1);
}

// ---------------------------------------------------------------------
// §13.3.4 + §13.12: the chunk-selection/consumption math lives in
// log_ring_next_chunk (host-tested, S1-S5); this body only sequences
// take_dropped -> next_chunk -> commit and owns the SDK send call.
void spp_log_thread(void const *argument) {
    (void)argument;

    for (;;) {
        osSignalWait(0x1, 200); /* 200 ms also drives the access-mode re-arm */
        spp_log_rearm_access_mode();

        if (s_inflight) {
            continue;
        }
        if (s_inflight_len) {
            /* Previous send confirmed (or short-written); advance the ring
               no further than the stack actually reported. */
            unsigned n = (s_done_len < s_inflight_len) ? s_done_len : s_inflight_len;
            log_ring_commit(&s_ring, s_inflight_base, n);
            s_inflight_len = 0;
        }
        if (!s_connected) {
            continue;
        }

        unsigned dropped = log_ring_take_dropped(&s_ring);
        unsigned n;
        int consumes_ring;

        if (dropped != 0) {
            /* Marker chunk: does not touch the ring, no mutex needed. */
            n = log_ring_next_chunk(&s_ring, dropped, s_contended, s_tx_slot,
                                    sizeof(s_tx_slot), &s_inflight_base,
                                    &consumes_ring);
        } else {
            osMutexWait(s_ring_mid, osWaitForever); /* only the log thread waits */
            n = log_ring_next_chunk(&s_ring, 0, s_contended, s_tx_slot,
                                    sizeof(s_tx_slot), &s_inflight_base,
                                    &consumes_ring);
            osMutexRelease(s_ring_mid);
        }
        s_inflight_len = consumes_ring ? n : 0;

        if (n == 0) {
            continue;
        }

        uint16_t len = (uint16_t)n;
        s_inflight = 1;
        if (btif_spp_write(s_dev, s_tx_slot, &len) != BT_STS_SUCCESS) {
            s_inflight = 0; /* retry next cycle */
            s_inflight_len = 0;
        }
    }
}

osThreadDef(spp_log_thread, osPriorityLow, 1, 1024, "spp_log");

}  // namespace

// ---------------------------------------------------------------------
// §13.3.3: the compute_log_tap side of COMPUTE_TRACE (compute_trace.h).
// try-lock only -- neither the compute thread (osPriorityBelowNormal) nor
// BesbtThread (osPriorityAboveNormal, RX handlers in mpi_ibrt_glue.cpp) may
// ever block here.
void compute_log_tap(const char *fmt, ...) {
    char line[192];
    va_list ap;
    int n;

    if (s_log_tid == NULL) {
        return; /* not initialized yet */
    }
    if (osMutexWait(s_ring_mid, 0) != osOK) { /* 0 = do not wait */
        s_contended++;
        return;
    }

    n = snprintf(line, sizeof(line), "#%u ", s_seq);
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - (unsigned)n, fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(line) - 1) {
            n = (int)sizeof(line) - 1;
        }
        s_seq++;
        log_ring_push(&s_ring, line, (unsigned)n);
    }
    osMutexRelease(s_ring_mid);
    osSignalSet(s_log_tid, 0x1);
}

// ---------------------------------------------------------------------
// §13.4.3: armed by mpi_ibrt_glue.cpp right after "[mpi] peer ok".
extern "C" void spp_log_enable_connectable(void) { s_scan_forced = 1; }

// ---------------------------------------------------------------------
// §13.2.2/§13.8: called once from BesbtThread, just before the event loop
// (services/bt_app/besmain.cpp, installed by scripts/install-into-sdk.sh).
extern "C" void spp_log_service_init(void) {
    log_ring_init(&s_ring);
    s_ring_mid = osMutexCreate(osMutex(spp_log_ring_mutex));
    s_log_tid = osThreadCreate(osThread(spp_log_thread), NULL);

    btif_sdp_record_param_t param;

    s_dev = btif_create_spp_device();
    btif_spp_init_rx_buf(s_dev, s_rx_buf, sizeof(s_rx_buf));
    s_dev->creditMutex = osMutexCreate(osMutex(spp_log_credit_mutex));

    s_record = btif_sdp_create_record();
    param.attrs = &g_log_spp_sdp_attributes[0];
    param.attr_count = ARRAY_SIZE(g_log_spp_sdp_attributes);
    param.COD = BTIF_COD_MAJOR_PERIPHERAL;
    btif_sdp_record_setup(s_record, &param);

    s_service = btif_create_spp_service();
    s_service->rf_service.serviceId = RFCOMM_CHANNEL_10; /* = 19 */
    s_service->numPorts = 0;
    btif_spp_service_setup(s_dev, s_service, s_record);

    s_dev->portType = BTIF_SPP_SERVER_PORT;
    s_dev->app_id = BTIF_APP_SPP_SERVER_ID_10;
    s_dev->spp_handle_data_event_func = spp_log_rx_discard;
    btif_spp_init_device(s_dev, kSppLogTxSlots,
                         osMutexCreate(osMutex(spp_log_mutex)));
    btif_spp_open(s_dev, NULL, spp_log_callback); /* NULL = server, listen */
}
