// Host-side TDD tests for the MPI adapter (adapters/mpi).
//
// The adapter must let an unmodified MPI benchmark source compile and run.
// On host, ranks are simulated in-process: a bootstrap seam sets rank/size
// before the benchmark calls MPI_Init (on target the IBRT link does this).
//
// Test list (t-wada style):
// [ ] bootstrap + MPI_Init: MPI_Comm_rank/MPI_Comm_size report rank 0 of 2
// [ ] rank 1 bootstrap: MPI_Comm_rank reports 1 (triangulation)
// [ ] MPI_Initialized: 0 before MPI_Init, 1 after
// [ ] MPI_Finalize: returns MPI_SUCCESS, MPI_Finalized flips to 1
// [ ] MPI_Send/MPI_Recv MPI_FLOAT: rank0 -> rank1 payload arrives intact
// [ ] MPI_Barrier: returns MPI_SUCCESS (2-rank sync via loopback transport)
// [ ] MPI_Wtime: monotonic non-negative float seconds
// [ ] MPI_Allreduce MPI_SUM on floats across 2 ranks

#include "test_framework.h"

#include "mpi.h"
#include "mpi_adapter.h"  // bootstrap seam (not part of the MPI API surface)

static void test_init_rank_size() {
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);

    int rank = -1;
    int size = -1;
    CHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
    CHECK(MPI_Comm_size(MPI_COMM_WORLD, &size) == MPI_SUCCESS);
    CHECK(rank == 0);
    CHECK(size == 2);

    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

int main() {
    RUN_TEST(test_init_rank_size);
    return testfw::summary();
}
