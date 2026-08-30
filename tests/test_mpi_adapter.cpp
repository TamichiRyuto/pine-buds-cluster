// Host-side TDD tests for the MPI adapter (adapters/mpi).
//
// The adapter must let an unmodified MPI benchmark source compile and run.
// On host, ranks are simulated in-process: a bootstrap seam sets rank/size
// before the benchmark calls MPI_Init (on target the IBRT link does this).
//
// Test list (t-wada style):
// [x] bootstrap + MPI_Init: MPI_Comm_rank/MPI_Comm_size report rank 0 of 2
// [x] rank 1 bootstrap: MPI_Comm_rank reports 1 (triangulation)
// [x] MPI_Initialized: 0 before MPI_Init, 1 after
// [x] MPI_Finalize: returns MPI_SUCCESS, MPI_Finalized flips to 1
// [x] MPI_Send/MPI_Recv MPI_FLOAT: rank0 -> rank1 payload arrives intact
// [x] MPI_Send beyond max payload bytes: rejected with an error (no overflow)
// [x] MPI_Send with queue full: rejected with an error (not silent success)
// [x] MPI_Recv with no matching message: error (loopback cannot block)
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

static void test_rank1_bootstrap() {
    mpi_adapter_bootstrap(1, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);

    int rank = -1;
    CHECK(MPI_Comm_rank(MPI_COMM_WORLD, &rank) == MPI_SUCCESS);
    CHECK(rank == 1);

    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static void test_initialized_finalized_flags() {
    // Fresh adapter state: bootstrap resets the init/finalize flags too.
    mpi_adapter_bootstrap(0, 2);

    int flag = -1;
    CHECK(MPI_Initialized(&flag) == MPI_SUCCESS);
    CHECK(flag == 0);
    CHECK(MPI_Finalized(&flag) == MPI_SUCCESS);
    CHECK(flag == 0);

    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    CHECK(MPI_Initialized(&flag) == MPI_SUCCESS);
    CHECK(flag == 1);
    CHECK(MPI_Finalized(&flag) == MPI_SUCCESS);
    CHECK(flag == 0);

    CHECK(MPI_Finalize() == MPI_SUCCESS);
    CHECK(MPI_Finalized(&flag) == MPI_SUCCESS);
    CHECK(flag == 1);
}

static void test_send_recv_float() {
    // Rank 0 sends; then we re-bootstrap as rank 1 in the same process and
    // receive. The adapter's loopback queue must survive re-bootstrap.
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    float out[3] = {1.0f, 2.0f, 3.0f};
    CHECK(MPI_Send(out, 3, MPI_FLOAT, 1, 42, MPI_COMM_WORLD) == MPI_SUCCESS);
    CHECK(MPI_Finalize() == MPI_SUCCESS);

    mpi_adapter_bootstrap(1, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    float in[3] = {0.0f, 0.0f, 0.0f};
    MPI_Status status;
    CHECK(MPI_Recv(in, 3, MPI_FLOAT, 0, 42, MPI_COMM_WORLD, &status) ==
          MPI_SUCCESS);
    CHECK_EQ_F(in[0], 1.0f);
    CHECK_EQ_F(in[1], 2.0f);
    CHECK_EQ_F(in[2], 3.0f);
    CHECK(status.MPI_SOURCE == 0);
    CHECK(status.MPI_TAG == 42);
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static void test_loopback_error_paths() {
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);

    // Payload larger than one queue slot must be rejected, not overflowed.
    float big[MPI_ADAPTER_MAX_PAYLOAD_BYTES / 4 + 1];
    const int big_count = (int)(sizeof(big) / sizeof(big[0]));
    for (int i = 0; i < big_count; ++i) big[i] = 0.0f;
    CHECK(MPI_Send(big, big_count, MPI_FLOAT, 1, 1, MPI_COMM_WORLD) ==
          MPI_ERR_COUNT);

    // The loopback cannot block, so an unmatched receive is an error.
    float in = 0.0f;
    MPI_Status status;
    CHECK(MPI_Recv(&in, 1, MPI_FLOAT, 1, 99, MPI_COMM_WORLD, &status) ==
          MPI_ERR_OTHER);

    // Filling every slot succeeds; one more send must report the full queue.
    float x = 7.0f;
    for (int i = 0; i < MPI_ADAPTER_QUEUE_SLOTS; ++i) {
        CHECK(MPI_Send(&x, 1, MPI_FLOAT, 1, i, MPI_COMM_WORLD) == MPI_SUCCESS);
    }
    CHECK(MPI_Send(&x, 1, MPI_FLOAT, 1, 999, MPI_COMM_WORLD) ==
          MPI_ERR_INTERN);
    CHECK(MPI_Finalize() == MPI_SUCCESS);

    // Drain as rank 1 so the queue is empty for whatever runs next.
    mpi_adapter_bootstrap(1, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    for (int i = 0; i < MPI_ADAPTER_QUEUE_SLOTS; ++i) {
        CHECK(MPI_Recv(&in, 1, MPI_FLOAT, 0, i, MPI_COMM_WORLD, &status) ==
              MPI_SUCCESS);
        CHECK_EQ_F(in, 7.0f);
    }
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

int main() {
    RUN_TEST(test_init_rank_size);
    RUN_TEST(test_rank1_bootstrap);
    RUN_TEST(test_initialized_finalized_flags);
    RUN_TEST(test_send_recv_float);
    RUN_TEST(test_loopback_error_paths);
    return testfw::summary();
}
