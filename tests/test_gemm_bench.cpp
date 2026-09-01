// Host-side TDD tests for the benchmark-shaped reference GEMM (bench/).
//
// bench/gemm_mpi_omp.cpp is the first test subject of the
// unmodified-benchmark strategy: a self-contained GEMM written against the
// standard MPI + OpenMP APIs, buildable unmodified against both real
// OpenMPI/libgomp (host golden reference) and our adapters. Tests compile it
// with -DGEMM_BENCH_NO_MAIN and drive gemm_bench_run directly.
//
// Verification is precision-independent: A = B = all ones, so every C
// element is exactly N and the global checksum is exactly N^3 in float32.
//
// Test list (t-wada style):
// [x] single rank (size 1): gemm_bench_run yields checksum N^3 (N=8)
// [x] two ranks under the pthread port: both ranks get checksum N^3, each
//     having computed only its own row half
// [x] N above the static capacity is rejected with a nonzero error
// [x] R9 (design §15.3): built with -fopenmp against our runtime and the
//     pthread omp port, a single rank with 2 threads still yields N^3 and
//     the worker thread ran the outlined loop body exactly once

#include "test_framework.h"

#include "mpi.h"
#include "mpi_adapter.h"
#include "mpi_thread_port.h"
#include "omp.h"
#include "omp_thread_port.h"

extern "C" int gemm_bench_run(int n, float *checksum_out);

static void test_single_rank_checksum() {
    mpi_adapter_bootstrap(0, 1);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);

    float checksum = 0.0f;
    CHECK(gemm_bench_run(8, &checksum) == 0);
    CHECK_EQ_F(checksum, 512.0f);  // 8^3

    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

// Rank threads record into globals; CHECKs stay on the main thread.
static int g_bench_rc[2] = {-1, -1};
static float g_bench_checksum[2] = {0.0f, 0.0f};

static void threaded_bench_body(int rank) {
    g_bench_rc[rank] = gemm_bench_run(8, &g_bench_checksum[rank]);
}

static void test_two_ranks_checksum() {
    // The Allreduce result can only reach 8^3 if both ranks contributed
    // their own half of the rows, so this also verifies the row split.
    mpi_adapter_bootstrap(0, 2);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    mpi_thread_port::run_two_ranks(&threaded_bench_body);
    CHECK(g_bench_rc[0] == 0);
    CHECK(g_bench_rc[1] == 0);
    CHECK_EQ_F(g_bench_checksum[0], 512.0f);
    CHECK_EQ_F(g_bench_checksum[1], 512.0f);
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static void test_oversize_n_rejected() {
    mpi_adapter_bootstrap(0, 1);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    float checksum = 0.0f;
    CHECK(gemm_bench_run(33, &checksum) != 0);
    CHECK(gemm_bench_run(0, &checksum) != 0);
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

static void test_r9_openmp_two_threads_single_rank() {
    mpi_adapter_bootstrap(0, 1);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);
    omp_thread_port::install();
    omp_set_num_threads(2);
    CHECK(omp_get_max_threads() == 2);

    float checksum = 0.0f;
    CHECK(gemm_bench_run(8, &checksum) == 0);
    CHECK_EQ_F(checksum, 512.0f);  // 8^3
    // The `#pragma omp parallel for` in the unmodified bench reached our
    // GOMP_parallel and forked the worker thread once.
    CHECK(omp_thread_port::g_starts == 1);
    CHECK(omp_thread_port::g_joins == 1);

    omp_set_port(nullptr);
    omp_set_num_threads(1);
    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

int main() {
    RUN_TEST(test_single_rank_checksum);
    RUN_TEST(test_two_ranks_checksum);
    RUN_TEST(test_oversize_n_rejected);
    RUN_TEST(test_r9_openmp_two_threads_single_rank);
    return testfw::summary();
}
