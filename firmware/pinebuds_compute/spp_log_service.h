// SDK-facing SPP log channel service (docs/design-ibrt-transport.md §13).
// Registers the PineBudsLog SPP server (§13.2), drains the compute_log_tap
// ring over RFCOMM on a dedicated thread (§13.3.4), and re-arms page-scan
// access mode once MPI bring-up hands off (§13.4). Target-only: never
// built on host (spp_log_service.cpp uses SDK headers freely).
#ifndef PINEBUDS_FIRMWARE_SPP_LOG_SERVICE_H
#define PINEBUDS_FIRMWARE_SPP_LOG_SERVICE_H

#ifdef __cplusplus
extern "C" {
#endif

// Registers the SDP record + RFCOMM server and starts the log-drain thread.
// Must run once, on BesbtThread, before the event loop -- installed via the
// besmain.cpp hook (design §13.8), not called directly from this header's
// includers.
void spp_log_service_init(void);

// Called by mpi_ibrt_glue.cpp right after it prints "[mpi] peer ok" (design
// §13.4.3, case C): arms the log thread's periodic re-assertion of
// BTIF_BAM_CONNECTABLE_ONLY so a PC can reconnect over the existing bond
// without disturbing the in-case TWS bring-up sequence.
void spp_log_enable_connectable(void);

#ifdef __cplusplus
}
#endif

#endif /* PINEBUDS_FIRMWARE_SPP_LOG_SERVICE_H */
