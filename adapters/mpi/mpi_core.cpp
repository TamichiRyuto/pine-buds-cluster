// Minimal MPI adapter core: static rank/size state set via the bootstrap
// seam, reported back through the MPI_* API subset.
#include "mpi.h"
#include "mpi_adapter.h"

#include <string.h>

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

    int mpi_datatype_size(MPI_Datatype datatype) {
        switch (datatype) {
            case MPI_FLOAT:
                return (int)sizeof(float);
            default:
                return (int)sizeof(float);
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

extern "C" int MPI_Comm_rank(MPI_Comm comm, int *rank) {
    (void)comm;
    *rank = g_rank;
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
