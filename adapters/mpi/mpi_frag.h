// Fragmentation / reassembly layer for the MPI adapter transport
// (docs/design-ibrt-transport.md §3.2, §4, §5). Splits a message larger
// than one wire frame into MPI_FRAG_PAYLOAD_BYTES chunks, reassembles them
// on the peer, and enforces credit-window flow control -- all through the
// injected mpi_frag_port, so this module stays target-independent and
// freestanding (no heap/STL/exceptions/double, gnu++98).
#ifndef PINEBUDS_ADAPTERS_MPI_FRAG_H
#define PINEBUDS_ADAPTERS_MPI_FRAG_H

#include "mpi_adapter.h"  // MPI_ADAPTER_MAX_PAYLOAD_BYTES

// Wire header layout (12 bytes, design §4 offset table):
//   0 kind(1) | 1 src(1) | 2 dst(1) | 3 msg_id(1) | 4 frag_idx(1) |
//   5 frag_cnt(1) | 6 total_len(u16) | 8 tag(int32)
// Both ends are the same little-endian Cortex-M4 binary, so a packed
// struct + memcpy serializes this without any endian conversion.
#define MPI_FRAG_HDR_BYTES     12
#define MPI_FRAG_PAYLOAD_BYTES 256
#define MPI_FRAG_FRAME_BYTES   (MPI_FRAG_HDR_BYTES + MPI_FRAG_PAYLOAD_BYTES)

#define MPI_FRAG_KIND_DATA 1
#define MPI_FRAG_KIND_ACK  2

#ifdef __cplusplus
extern "C" {
#endif

// Port seam: wire I/O and credit-window primitives, injected so this module
// never touches a transport or an RTOS primitive directly.
typedef struct mpi_frag_port {
    int  (*emit)(const void *frame, int frame_len);
    int  (*deliver)(int source, int dest, int tag, const void *buf,
                    int byte_len);
    int  (*acquire_credit)(void);   /* 0 = granted, nonzero = denied */
    void (*release_credit)(void);
} mpi_frag_port;

// Copies *port and resets all module state: per-src msg_id counters,
// reassembly slots, and the tx/rx/err counters.
void mpi_frag_init(const mpi_frag_port *port);

// Fragments buf (from src, to dest, with tag) into one or more DATA frames
// and emits them through the port, gated by one acquire_credit() call per
// fragment. Returns MPI_ERR_COUNT for byte_len outside (0,
// MPI_ADAPTER_MAX_PAYLOAD_BYTES], MPI_ERR_OTHER if credit is denied or a
// frame fails to emit (nothing further is sent once that happens).
int mpi_frag_send(int src, int dest, int tag, const void *buf, int byte_len);

// Feeds one received wire frame into the module. An ACK frame releases one
// credit. A DATA frame is copied into the per-source reassembly slot and,
// once the last fragment of the message has arrived, delivered through the
// port. Only one message per source may be in flight (design §4): a DATA
// frame whose msg_id does not match an in-progress reassembly for its
// source is rejected (MPI_ERR_INTERN) without disturbing that reassembly.
int mpi_frag_on_frame(const void *frame, int frame_len);

void mpi_frag_counters(unsigned *tx, unsigned *rx, unsigned *err);

#ifdef __cplusplus
}
#endif

#endif /* PINEBUDS_ADAPTERS_MPI_FRAG_H */
