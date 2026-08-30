// Minimal MPI adapter core: static rank/size state set via the bootstrap
// seam, reported back through the MPI_* API subset.
#include "mpi.h"
#include "mpi_adapter.h"

namespace {
    int g_rank = 0;
    int g_size = 1;
    int g_initialized = 0;
    int g_finalized = 0;
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
