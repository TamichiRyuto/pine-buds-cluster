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
// [ ] single rank (size 1): gemm_bench_run yields checksum N^3 (N=8)
// [ ] two ranks under the pthread port: both ranks get checksum N^3, each
//     having computed only its own row half
// [ ] N above the static capacity is rejected with a nonzero error

#include "test_framework.h"

#include "mpi.h"
#include "mpi_adapter.h"
#include "mpi_thread_port.h"

extern "C" int gemm_bench_run(int n, float *checksum_out);

static void test_single_rank_checksum() {
    mpi_adapter_bootstrap(0, 1);
    CHECK(MPI_Init(0, 0) == MPI_SUCCESS);

    float checksum = 0.0f;
    CHECK(gemm_bench_run(8, &checksum) == 0);
    CHECK_EQ_F(checksum, 512.0f);  // 8^3

    CHECK(MPI_Finalize() == MPI_SUCCESS);
}

int main() {
    RUN_TEST(test_single_rank_checksum);
    return testfw::summary();
}
