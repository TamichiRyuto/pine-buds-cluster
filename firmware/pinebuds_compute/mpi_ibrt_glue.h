// Firmware glue between adapters/mpi and the IBRT TWS link
// (docs/design-ibrt-transport.md §6). Registers the mpi_frag/mpi_adapter
// seams against the SDK's TWS custom-command channel and runs the M-T1 /
// M-T2 / M-T3 device milestones (design §9) sequentially, in one boot, on
// a dedicated compute thread. Target-only: never built on host.
#ifndef PINEBUDS_FIRMWARE_MPI_IBRT_GLUE_H
#define PINEBUDS_FIRMWARE_MPI_IBRT_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

// Registers the TWS cmd table (TWS_CMD_OTA slot, cmdcode 0x8201) and spawns
// the compute thread. Returns immediately -- does not block app_init.
void mpi_ibrt_glue_start(void);

// Design §14: 5-tap entry point (apps/main/key_handler.cpp, app_thread).
// Starts a GEMM-MPI re-run on both buds; ignored while one is in progress
// or before the boot run has finished. Never blocks.
void mpi_ibrt_trigger_run(void);

#ifdef __cplusplus
}
#endif

#endif  /* PINEBUDS_FIRMWARE_MPI_IBRT_GLUE_H */
