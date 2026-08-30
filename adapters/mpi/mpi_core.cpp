// Minimal MPI adapter core: static rank/size state set via the bootstrap
// seam, reported back through the MPI_* API subset.
#include "mpi.h"
#include "mpi_adapter.h"

namespace {
    int g_rank = 0;
    int g_size = 1;
}

extern "C" void mpi_adapter_bootstrap(int rank, int size) {
    g_rank = rank;
    g_size = size;
}

extern "C" int MPI_Init(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
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
    return MPI_SUCCESS;
}
