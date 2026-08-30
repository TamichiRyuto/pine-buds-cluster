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

// Message-level send seam (docs/design-ibrt-transport.md §3.1). Once
// installed, an MPI_Send whose dest is not self skips the local loopback
// queue entirely and goes only through this send. Deviation from the design
// doc: send takes an explicit src first parameter, since on target the
// transport's own identity (not just the calling rank) matters for framing.
typedef struct mpi_adapter_transport {
    int (*send)(int src, int dest, int tag, const void *buf, int byte_len);
} mpi_adapter_transport;

void mpi_adapter_set_transport(const mpi_adapter_transport *transport);

// Receive-side injection point, called from the transport's RX context (on
// target, the IBRT callback running on BesbtThread rather than the compute
// thread). Enqueues into the local loopback queue and wakes any waiting
// MPI_Recv; never blocks beyond the port's own lock.
int mpi_adapter_deliver(int source, int dest, int tag,
                        const void *buf, int byte_len);

#ifdef __cplusplus
}
#endif

#endif /* PINEBUDS_ADAPTERS_MPI_ADAPTER_H */
