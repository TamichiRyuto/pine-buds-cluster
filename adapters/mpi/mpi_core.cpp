// Minimal MPI adapter core: static rank/size state set via the bootstrap
// seam, reported back through the MPI_* API subset.
#include "mpi.h"
#include "mpi_adapter.h"

#include <string.h>
#include <time.h>

namespace {
    int g_rank = 0;
    int g_size = 1;
    int g_initialized = 0;
    int g_finalized = 0;

    // In-process loopback transport: a static queue of pending messages.
    // Survives mpi_adapter_bootstrap (only rank/size/init flags reset there)
    // so a send under one rank can be received after re-bootstrapping as
    // another rank in the same process.
    struct QueueSlot {
        int in_use;
        int source;
        int dest;
        int tag;
        int byte_len;
        char payload[MPI_ADAPTER_MAX_PAYLOAD_BYTES];
    };

    QueueSlot g_queue[MPI_ADAPTER_QUEUE_SLOTS];

    // Concurrency port: a copy of the caller's struct (which may be
    // stack-allocated) plus an installed flag. NULL/not-installed means
    // sequential mode, matching the original non-blocking behavior.
    mpi_adapter_port g_port;
    int g_port_installed = 0;

    // Transport seam: a copy of the caller's struct (mirrors g_port /
    // g_port_installed above) plus an installed flag. Not installed means
    // the local loopback queue handles every destination, matching the
    // original behavior (docs/design-ibrt-transport.md §3.1).
    mpi_adapter_transport g_transport;
    int g_transport_installed = 0;

    // Calling rank: the port's self_rank() when installed (per-thread
    // identity), else the bootstrap rank g_rank.
    int current_rank(void) {
        if (g_port_installed) {
            return g_port.self_rank();
        }
        return g_rank;
    }

    int mpi_datatype_size(MPI_Datatype datatype) {
        switch (datatype) {
            case MPI_FLOAT:
                return (int)sizeof(float);
            default:
                return (int)sizeof(float);
        }
    }

    // Host time source, isolated so a target port can swap this for a hal
    // timer without touching the MPI_Wtime API surface above.
    double wtime_seconds(void) {
        return (double)clock() / (double)CLOCKS_PER_SEC;
    }

    // Queue primitives shared by the sequential and port-locked paths.
    // Callers hold the port lock when one is installed.
    int enqueue_message(int src, int dest, int tag, const void *buf,
                        int byte_len) {
        for (int i = 0; i < MPI_ADAPTER_QUEUE_SLOTS; ++i) {
            if (!g_queue[i].in_use) {
                g_queue[i].in_use = 1;
                g_queue[i].source = src;
                g_queue[i].dest = dest;
                g_queue[i].tag = tag;
                g_queue[i].byte_len = byte_len;
                memcpy(g_queue[i].payload, buf, byte_len);
                return 1;
            }
        }
        return 0;
    }

    // Tag range reserved for MPI_Allreduce's and MPI_Barrier's internal
    // Send/Recv pairs. User code must not use tags in this range
    // (0x7FFF0000..0x7FFF0003).
    const int kAllreduceTag = 0x7FFF0000;
    const int kBarrierTag = 0x7FFF0002;

    int dequeue_match(int dest, int source, int tag, void *buf,
                      MPI_Status *status) {
        for (int i = 0; i < MPI_ADAPTER_QUEUE_SLOTS; ++i) {
            if (g_queue[i].in_use && g_queue[i].dest == dest &&
                g_queue[i].source == source && g_queue[i].tag == tag) {
                memcpy(buf, g_queue[i].payload, g_queue[i].byte_len);
                status->MPI_SOURCE = g_queue[i].source;
                status->MPI_TAG = g_queue[i].tag;
                g_queue[i].in_use = 0;
                return 1;
            }
        }
        return 0;
    }

    // Nonblocking request table. Handle = slot index + 1, so 0 stays
    // MPI_REQUEST_NULL.
    enum RequestKind { kRequestSend, kRequestRecv };

    struct RequestSlot {
        int in_use;
        RequestKind kind;
        int done;
        void *buf;
        int source;    // recv: peer to match; send: unused
        int tag;
        int owner_rank;  // rank that posted this request
    };

    RequestSlot g_requests[MPI_ADAPTER_MAX_REQUESTS];

    int alloc_request_slot(void) {
        for (int i = 0; i < MPI_ADAPTER_MAX_REQUESTS; ++i) {
            if (!g_requests[i].in_use) {
                g_requests[i].in_use = 1;
                return i;
            }
        }
        return -1;
    }

    // Caller holds the port lock when one is installed.
    void try_complete(int slot_index) {
        RequestSlot *slot = &g_requests[slot_index];
        if (slot->done) {
            return;
        }
        if (slot->kind == kRequestSend) {
            slot->done = 1;
            return;
        }
        MPI_Status status;
        if (dequeue_match(slot->owner_rank, slot->source, slot->tag,
                          slot->buf, &status)) {
            slot->source = status.MPI_SOURCE;
            slot->tag = status.MPI_TAG;
            slot->done = 1;
        }
    }
}

extern "C" void mpi_adapter_bootstrap(int rank, int size) {
    g_rank = rank;
    g_size = size;
    g_initialized = 0;
    g_finalized = 0;
}

extern "C" int MPI_Init(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    g_initialized = 1;
    return MPI_SUCCESS;
}

extern "C" int MPI_Initialized(int *flag) {
    *flag = g_initialized;
    return MPI_SUCCESS;
}

extern "C" int MPI_Finalized(int *flag) {
    *flag = g_finalized;
    return MPI_SUCCESS;
}

extern "C" void mpi_adapter_set_port(const mpi_adapter_port *port) {
    if (port == 0) {
        g_port_installed = 0;
        return;
    }
    g_port = *port;
    g_port_installed = 1;
}

extern "C" void mpi_adapter_set_transport(const mpi_adapter_transport *transport) {
    if (transport == 0) {
        g_transport_installed = 0;
        return;
    }
    g_transport = *transport;
    g_transport_installed = 1;
}

extern "C" int MPI_Comm_rank(MPI_Comm comm, int *rank) {
    (void)comm;
    *rank = current_rank();
    return MPI_SUCCESS;
}

extern "C" int MPI_Comm_size(MPI_Comm comm, int *size) {
    (void)comm;
    *size = g_size;
    return MPI_SUCCESS;
}

extern "C" int MPI_Finalize(void) {
    g_finalized = 1;
    return MPI_SUCCESS;
}

extern "C" int MPI_Send(const void *buf, int count, MPI_Datatype datatype,
                         int dest, int tag, MPI_Comm comm) {
    (void)comm;
    int byte_len = count * mpi_datatype_size(datatype);

    if (byte_len > MPI_ADAPTER_MAX_PAYLOAD_BYTES) {
        return MPI_ERR_COUNT;
    }

    // Transport seam: once installed, a send to a peer (not self) skips the
    // local loopback queue entirely and goes only through the wire. Self
    // sends and the no-transport path are untouched below.
    if (g_transport_installed && dest != current_rank()) {
        return g_transport.send(current_rank(), dest, tag, buf, byte_len);
    }

    if (g_port_installed) {
        g_port.lock();
        int queued = enqueue_message(current_rank(), dest, tag, buf, byte_len);
        if (queued) {
            g_port.wake();
        }
        g_port.unlock();
        return queued ? MPI_SUCCESS : MPI_ERR_INTERN;
    }

    return enqueue_message(g_rank, dest, tag, buf, byte_len) ? MPI_SUCCESS
                                                             : MPI_ERR_INTERN;
}

extern "C" int MPI_Recv(void *buf, int count, MPI_Datatype datatype,
                         int source, int tag, MPI_Comm comm,
                         MPI_Status *status) {
    (void)comm;
    (void)count;
    (void)datatype;

    if (g_port_installed) {
        g_port.lock();
        while (!dequeue_match(current_rank(), source, tag, buf, status)) {
            g_port.wait();
        }
        g_port.wake();
        g_port.unlock();
        return MPI_SUCCESS;
    }

    return dequeue_match(g_rank, source, tag, buf, status) ? MPI_SUCCESS
                                                           : MPI_ERR_OTHER;
}

// Receive-side injection point for the transport seam. Called from the
// transport's RX context, which on target is BesbtThread, not the compute
// thread that owns g_port.self_rank() -- that is why source/dest arrive as
// explicit arguments here instead of being read from current_rank(). Must
// never block beyond the short lock below.
extern "C" int mpi_adapter_deliver(int source, int dest, int tag,
                                   const void *buf, int byte_len) {
    if (byte_len > MPI_ADAPTER_MAX_PAYLOAD_BYTES) {
        return MPI_ERR_COUNT;
    }

    if (g_port_installed) {
        g_port.lock();
        int queued = enqueue_message(source, dest, tag, buf, byte_len);
        if (queued) {
            g_port.wake();
        }
        g_port.unlock();
        return queued ? MPI_SUCCESS : MPI_ERR_INTERN;
    }

    return enqueue_message(source, dest, tag, buf, byte_len) ? MPI_SUCCESS
                                                              : MPI_ERR_INTERN;
}

extern "C" int MPI_Barrier(MPI_Comm comm) {
    (void)comm;
    // Sequential host loopback runs one rank at a time, so there is nothing
    // to synchronize with; without a port, keep returning immediately.
    if (!g_port_installed || g_size < 2) {
        return MPI_SUCCESS;
    }

    // Two-rank token rendezvous, built on the existing Send/Recv primitives,
    // mirroring MPI_Allreduce's reduce-to-root-then-broadcast shape.
    float token = 0.0f;
    if (current_rank() != 0) {
        int rc = MPI_Send(&token, 1, MPI_FLOAT, 0, kBarrierTag, comm);
        if (rc != MPI_SUCCESS) {
            return rc;
        }
        MPI_Status status;
        return MPI_Recv(&token, 1, MPI_FLOAT, 0, kBarrierTag + 1, comm,
                        &status);
    }

    MPI_Status status;
    int rc = MPI_Recv(&token, 1, MPI_FLOAT, 1, kBarrierTag, comm, &status);
    if (rc != MPI_SUCCESS) {
        return rc;
    }
    return MPI_Send(&token, 1, MPI_FLOAT, 1, kBarrierTag + 1, comm);
}

extern "C" double MPI_Wtime(void) {
    return wtime_seconds();
}

extern "C" int MPI_Allreduce(const void *sendbuf, void *recvbuf, int count,
                              MPI_Datatype datatype, MPI_Op op,
                              MPI_Comm comm) {
    (void)op;  // only MPI_SUM is implemented, which is all that's needed
    int byte_len = count * mpi_datatype_size(datatype);

    if (g_size == 1) {
        memcpy(recvbuf, sendbuf, byte_len);
        return MPI_SUCCESS;
    }

    // Two-rank reduce-to-root-then-broadcast, built on the existing
    // Send/Recv primitives so it works under both sequential and threaded
    // (mpi_thread_port) modes.
    if (current_rank() != 0) {
        int rc = MPI_Send(sendbuf, count, datatype, 0, kAllreduceTag, comm);
        if (rc != MPI_SUCCESS) {
            return rc;
        }
        MPI_Status status;
        return MPI_Recv(recvbuf, count, datatype, 0, kAllreduceTag + 1, comm,
                        &status);
    }

    memcpy(recvbuf, sendbuf, byte_len);

    static char scratch[MPI_ADAPTER_MAX_PAYLOAD_BYTES];
    MPI_Status status;
    int rc = MPI_Recv(scratch, count, datatype, 1, kAllreduceTag, comm,
                      &status);
    if (rc != MPI_SUCCESS) {
        return rc;
    }

    float *out = (float *)recvbuf;
    const float *peer = (const float *)scratch;
    for (int i = 0; i < count; ++i) {
        out[i] = out[i] + peer[i];
    }

    return MPI_Send(recvbuf, count, datatype, 1, kAllreduceTag + 1, comm);
}

extern "C" int MPI_Isend(const void *buf, int count, MPI_Datatype datatype,
                          int dest, int tag, MPI_Comm comm,
                          MPI_Request *request) {
    // Eager: perform the same enqueue MPI_Send does now. The message is
    // copied into the queue, so completion is immediate.
    int rc = MPI_Send(buf, count, datatype, dest, tag, comm);
    if (rc != MPI_SUCCESS) {
        return rc;
    }

    int slot_index = alloc_request_slot();
    if (slot_index < 0) {
        return MPI_ERR_INTERN;
    }
    g_requests[slot_index].kind = kRequestSend;
    g_requests[slot_index].done = 1;
    g_requests[slot_index].buf = 0;
    g_requests[slot_index].source = current_rank();
    g_requests[slot_index].tag = tag;
    g_requests[slot_index].owner_rank = current_rank();
    *request = (MPI_Request)(slot_index + 1);
    return MPI_SUCCESS;
}

extern "C" int MPI_Irecv(void *buf, int count, MPI_Datatype datatype,
                          int source, int tag, MPI_Comm comm,
                          MPI_Request *request) {
    (void)count;
    (void)datatype;
    (void)comm;

    int slot_index = alloc_request_slot();
    if (slot_index < 0) {
        return MPI_ERR_INTERN;
    }
    g_requests[slot_index].kind = kRequestRecv;
    g_requests[slot_index].done = 0;
    g_requests[slot_index].buf = buf;
    g_requests[slot_index].source = source;
    g_requests[slot_index].tag = tag;
    g_requests[slot_index].owner_rank = current_rank();
    *request = (MPI_Request)(slot_index + 1);
    return MPI_SUCCESS;
}

extern "C" int MPI_Wait(MPI_Request *request, MPI_Status *status) {
    if (*request == MPI_REQUEST_NULL) {
        return MPI_SUCCESS;
    }

    int slot_index = (int)*request - 1;
    RequestSlot *slot = &g_requests[slot_index];

    if (g_port_installed) {
        g_port.lock();
        while (!slot->done) {
            try_complete(slot_index);
            if (!slot->done) {
                g_port.wait();
            }
        }
        g_port.unlock();
    } else {
        try_complete(slot_index);
        if (!slot->done) {
            return MPI_ERR_OTHER;
        }
    }

    if (status != 0) {
        if (slot->kind == kRequestSend) {
            status->MPI_SOURCE = slot->owner_rank;
        } else {
            status->MPI_SOURCE = slot->source;
        }
        status->MPI_TAG = slot->tag;
    }

    slot->in_use = 0;
    *request = MPI_REQUEST_NULL;
    return MPI_SUCCESS;
}

extern "C" int MPI_Waitall(int count, MPI_Request *requests,
                            MPI_Status *statuses) {
    int first_rc = MPI_SUCCESS;
    for (int i = 0; i < count; ++i) {
        MPI_Status *status = (statuses != 0) ? &statuses[i] : 0;
        int rc = MPI_Wait(&requests[i], status);
        if (first_rc == MPI_SUCCESS) {
            first_rc = rc;
        }
    }
    return first_rc;
}
