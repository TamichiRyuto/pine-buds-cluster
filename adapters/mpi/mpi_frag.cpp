// Fragmentation / reassembly implementation. See mpi_frag.h and
// docs/design-ibrt-transport.md §3.2/§4/§5 for the contract.
#include "mpi_frag.h"
#include "mpi.h"  // MPI_SUCCESS / MPI_ERR_* codes

#include <string.h>

namespace {
    // Wire header, 12 bytes (mpi_frag.h). Natural alignment already packs
    // this with no padding on both the host (x86-64) and the target
    // (Cortex-M4): 6 bytes of uint8 fields, then a naturally-aligned u16,
    // then a naturally-aligned int32. Pinned below rather than assumed.
    struct mpi_frag_hdr {
        unsigned char kind;
        unsigned char src;
        unsigned char dst;
        unsigned char msg_id;
        unsigned char frag_idx;
        unsigned char frag_cnt;
        unsigned short total_len;
        int tag;
    };

    typedef char mpi_frag_hdr_size_check[(sizeof(mpi_frag_hdr) == 12) ? 1
                                                                       : -1];

    // Design §4 limits reassembly to one message in flight per source, and
    // this cluster only ever has two ranks (0 and 1), so per-source state
    // is a flat 2-entry table indexed directly by src.
    const int kMaxPeers = 2;

    struct ReassemblyState {
        int active;
        unsigned char msg_id;
        int frag_cnt;
        int received_count;
        int total_len;
        int src;
        int dst;
        int tag;
        unsigned char buf[MPI_ADAPTER_MAX_PAYLOAD_BYTES];
    };

    mpi_frag_port g_port;

    unsigned char g_next_msg_id[kMaxPeers];
    ReassemblyState g_reassembly[kMaxPeers];

    unsigned g_tx = 0;
    unsigned g_rx = 0;
    unsigned g_err = 0;

    void reset_reassembly(ReassemblyState *state) {
        state->active = 0;
        state->msg_id = 0;
        state->frag_cnt = 0;
        state->received_count = 0;
        state->total_len = 0;
        state->src = 0;
        state->dst = 0;
        state->tag = 0;
    }
}

extern "C" void mpi_frag_init(const mpi_frag_port *port) {
    g_port = *port;

    for (int i = 0; i < kMaxPeers; ++i) {
        g_next_msg_id[i] = 0;
        reset_reassembly(&g_reassembly[i]);
    }

    g_tx = 0;
    g_rx = 0;
    g_err = 0;
}

extern "C" int mpi_frag_send(int src, int dest, int tag, const void *buf,
                             int byte_len) {
    if (byte_len <= 0 || byte_len > MPI_ADAPTER_MAX_PAYLOAD_BYTES) {
        return MPI_ERR_COUNT;
    }

    int frag_cnt =
        (byte_len + MPI_FRAG_PAYLOAD_BYTES - 1) / MPI_FRAG_PAYLOAD_BYTES;

    unsigned char msg_id = g_next_msg_id[src];
    g_next_msg_id[src] = (unsigned char)(msg_id + 1);

    static unsigned char frame[MPI_FRAG_FRAME_BYTES];
    const unsigned char *src_bytes = (const unsigned char *)buf;
    int offset = 0;

    for (int frag_idx = 0; frag_idx < frag_cnt; ++frag_idx) {
        int chunk_len = byte_len - offset;
        if (chunk_len > MPI_FRAG_PAYLOAD_BYTES) {
            chunk_len = MPI_FRAG_PAYLOAD_BYTES;
        }

        // Credit gates the fragment before anything is built or emitted:
        // a denial must leave the wire untouched (design §5).
        if (g_port.acquire_credit() != 0) {
            return MPI_ERR_OTHER;
        }

        mpi_frag_hdr hdr;
        hdr.kind = MPI_FRAG_KIND_DATA;
        hdr.src = (unsigned char)src;
        hdr.dst = (unsigned char)dest;
        hdr.msg_id = msg_id;
        hdr.frag_idx = (unsigned char)frag_idx;
        hdr.frag_cnt = (unsigned char)frag_cnt;
        hdr.total_len = (unsigned short)byte_len;
        hdr.tag = tag;

        memcpy(frame, &hdr, MPI_FRAG_HDR_BYTES);
        memcpy(frame + MPI_FRAG_HDR_BYTES, src_bytes + offset, chunk_len);

        if (g_port.emit(frame, MPI_FRAG_HDR_BYTES + chunk_len) != 0) {
            return MPI_ERR_OTHER;
        }
        ++g_tx;

        offset += chunk_len;
    }

    return MPI_SUCCESS;
}

extern "C" int mpi_frag_on_frame(const void *frame, int frame_len) {
    if (frame_len < MPI_FRAG_HDR_BYTES) {
        ++g_err;
        return MPI_ERR_COUNT;
    }

    mpi_frag_hdr hdr;
    memcpy(&hdr, frame, MPI_FRAG_HDR_BYTES);

    if (hdr.kind == MPI_FRAG_KIND_ACK) {
        g_port.release_credit();
        ++g_rx;
        return MPI_SUCCESS;
    }

    if (hdr.kind != MPI_FRAG_KIND_DATA) {
        ++g_err;
        return MPI_ERR_INTERN;
    }

    int src = hdr.src;
    if (src < 0 || src >= kMaxPeers) {
        ++g_err;
        return MPI_ERR_INTERN;
    }

    ReassemblyState *state = &g_reassembly[src];

    if (state->active && state->msg_id != hdr.msg_id) {
        // Interleaved message from the same source (design §4): count the
        // error but leave the in-progress reassembly untouched -- it must
        // still complete intact.
        ++g_err;
        return MPI_ERR_INTERN;
    }

    int chunk_len = frame_len - MPI_FRAG_HDR_BYTES;
    int frag_offset = (int)hdr.frag_idx * MPI_FRAG_PAYLOAD_BYTES;
    if (chunk_len < 0 ||
        frag_offset + chunk_len > MPI_ADAPTER_MAX_PAYLOAD_BYTES) {
        ++g_err;
        return MPI_ERR_INTERN;
    }

    if (!state->active) {
        // First fragment of a new message starts reassembly.
        state->active = 1;
        state->msg_id = hdr.msg_id;
        state->frag_cnt = hdr.frag_cnt;
        state->received_count = 0;
        state->total_len = hdr.total_len;
        state->src = hdr.src;
        state->dst = hdr.dst;
        state->tag = hdr.tag;
    }

    memcpy(state->buf + frag_offset, (const unsigned char *)frame +
                                          MPI_FRAG_HDR_BYTES,
          chunk_len);
    ++state->received_count;
    ++g_rx;

    // Per-fragment ACK (design §5): matches credit-per-fragment flow
    // control. Echoes src/dst swapped plus this fragment's msg_id/frag_idx.
    mpi_frag_hdr ack;
    ack.kind = MPI_FRAG_KIND_ACK;
    ack.src = hdr.dst;
    ack.dst = hdr.src;
    ack.msg_id = hdr.msg_id;
    ack.frag_idx = hdr.frag_idx;
    ack.frag_cnt = 0;
    ack.total_len = 0;
    ack.tag = 0;

    unsigned char ack_frame[MPI_FRAG_HDR_BYTES];
    memcpy(ack_frame, &ack, MPI_FRAG_HDR_BYTES);
    g_port.emit(ack_frame, MPI_FRAG_HDR_BYTES);

    if (state->received_count < state->frag_cnt) {
        return MPI_SUCCESS;
    }

    int rc = g_port.deliver(state->src, state->dst, state->tag, state->buf,
                            state->total_len);
    state->active = 0;
    return rc;
}

extern "C" void mpi_frag_counters(unsigned *tx, unsigned *rx, unsigned *err) {
    *tx = g_tx;
    *rx = g_rx;
    *err = g_err;
}
