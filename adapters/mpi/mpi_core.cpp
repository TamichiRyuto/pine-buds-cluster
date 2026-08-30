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

    if (g_port_installed) {
        g_port.lock();
        if (byte_len > MPI_ADAPTER_MAX_PAYLOAD_BYTES) {
            g_port.unlock();
            return MPI_ERR_COUNT;
        }
        for (int i = 0; i < MPI_ADAPTER_QUEUE_SLOTS; ++i) {
            if (!g_queue[i].in_use) {
                g_queue[i].in_use = 1;
                g_queue[i].source = current_rank();
                g_queue[i].dest = dest;
                g_queue[i].tag = tag;
                g_queue[i].byte_len = byte_len;
                memcpy(g_queue[i].payload, buf, byte_len);
                g_port.wake();
                g_port.unlock();
                return MPI_SUCCESS;
            }
        }
        g_port.unlock();
        return MPI_ERR_INTERN;
    }

    if (byte_len > MPI_ADAPTER_MAX_PAYLOAD_BYTES) {
        return MPI_ERR_COUNT;
    }

    for (int i = 0; i < MPI_ADAPTER_QUEUE_SLOTS; ++i) {
        if (!g_queue[i].in_use) {
            g_queue[i].in_use = 1;
            g_queue[i].source = g_rank;
            g_queue[i].dest = dest;
            g_queue[i].tag = tag;
            g_queue[i].byte_len = byte_len;
            memcpy(g_queue[i].payload, buf, byte_len);
            return MPI_SUCCESS;
        }
    }
    return MPI_ERR_INTERN;
}

extern "C" int MPI_Recv(void *buf, int count, MPI_Datatype datatype,
                         int source, int tag, MPI_Comm comm,
                         MPI_Status *status) {
    (void)comm;
    (void)count;
    (void)datatype;

    if (g_port_installed) {
        g_port.lock();
        for (;;) {
            int dest_rank = current_rank();
            int found = 0;
            for (int i = 0; i < MPI_ADAPTER_QUEUE_SLOTS; ++i) {
                if (g_queue[i].in_use && g_queue[i].dest == dest_rank &&
                    g_queue[i].source == source && g_queue[i].tag == tag) {
                    memcpy(buf, g_queue[i].payload, g_queue[i].byte_len);
                    status->MPI_SOURCE = g_queue[i].source;
                    status->MPI_TAG = g_queue[i].tag;
                    g_queue[i].in_use = 0;
                    g_port.wake();
                    found = 1;
                    break;
                }
            }
            if (found) {
                g_port.unlock();
                return MPI_SUCCESS;
            }
            g_port.wait();
        }
    }

    for (int i = 0; i < MPI_ADAPTER_QUEUE_SLOTS; ++i) {
        if (g_queue[i].in_use && g_queue[i].dest == g_rank &&
            g_queue[i].source == source && g_queue[i].tag == tag) {
            memcpy(buf, g_queue[i].payload, g_queue[i].byte_len);
            status->MPI_SOURCE = g_queue[i].source;
            status->MPI_TAG = g_queue[i].tag;
            g_queue[i].in_use = 0;
            return MPI_SUCCESS;
        }
    }
    return MPI_ERR_OTHER;
}

extern "C" int MPI_Barrier(MPI_Comm comm) {
    (void)comm;
    // Sequential host loopback runs one rank at a time, so there is nothing
    // to synchronize with; the target IBRT transport implements real sync.
    return MPI_SUCCESS;
}

extern "C" double MPI_Wtime(void) {
    return wtime_seconds();
}
