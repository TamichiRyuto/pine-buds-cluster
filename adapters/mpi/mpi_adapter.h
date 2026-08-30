// Bootstrap seam for the MPI adapter. Not part of the MPI API surface.
// On host, tests call this to inject rank/size before MPI_Init.
// On target, the IBRT link performs the equivalent role.
#ifndef PINEBUDS_ADAPTERS_MPI_ADAPTER_H
#define PINEBUDS_ADAPTERS_MPI_ADAPTER_H

// Loopback transport limits, shared with mpi_core.cpp and exposed for tests.
#define MPI_ADAPTER_QUEUE_SLOTS 8
#define MPI_ADAPTER_MAX_PAYLOAD_BYTES 512

#ifdef __cplusplus
extern "C" {
#endif

void mpi_adapter_bootstrap(int rank, int size);

#ifdef __cplusplus
}
#endif

#endif /* PINEBUDS_ADAPTERS_MPI_ADAPTER_H */
