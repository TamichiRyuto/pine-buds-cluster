// Bootstrap seam for the MPI adapter. Not part of the MPI API surface.
// On host, tests call this to inject rank/size before MPI_Init.
// On target, the IBRT link performs the equivalent role.
#ifndef PINEBUDS_ADAPTERS_MPI_ADAPTER_H
#define PINEBUDS_ADAPTERS_MPI_ADAPTER_H

// Loopback transport limits, shared with mpi_core.cpp and exposed for tests.
#define MPI_ADAPTER_QUEUE_SLOTS 8
#define MPI_ADAPTER_MAX_PAYLOAD_BYTES 512
#define MPI_ADAPTER_MAX_REQUESTS 8

#ifdef __cplusplus
extern "C" {
#endif

void mpi_adapter_bootstrap(int rank, int size);

// Concurrency port seam: lets a host pthread harness (or, on target, an IBRT
// loop) give the adapter real blocking semantics without pulling threads or
// STL into this freestanding core. NULL restores sequential (non-blocking)
// mode.
typedef struct mpi_adapter_port {
    int  (*self_rank)(void);  /* calling rank; overrides the bootstrap rank */
    void (*lock)(void);
    void (*unlock)(void);
    void (*wait)(void);       /* condition wait: atomically unlock+wait+relock */
    void (*wake)(void);       /* wake all waiters */
} mpi_adapter_port;

void mpi_adapter_set_port(const mpi_adapter_port *port);

#ifdef __cplusplus
}
#endif

#endif /* PINEBUDS_ADAPTERS_MPI_ADAPTER_H */
