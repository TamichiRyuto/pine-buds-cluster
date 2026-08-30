// Host-side TDD tests for the OpenMP Stage-1 stub (adapters/omp).
//
// Stage 1 of the unmodified-benchmark strategy: provide omp.h so sources
// including it compile, with pragmas ignored and everything sequential.
// Stage 2 (-fopenmp + custom GOMP runtime on cp_accel) comes later.
//
// Test list (t-wada style):
// [x] omp_get_num_threads returns 1, omp_get_thread_num returns 0
// [x] omp_get_max_threads / omp_get_num_procs return 1
// [x] omp_set_num_threads is accepted and ignored (still 1 thread)
// [ ] omp_get_wtime: monotonic non-negative seconds

#include "test_framework.h"

#include "omp.h"

static void test_sequential_identity() {
    CHECK(omp_get_num_threads() == 1);
    CHECK(omp_get_thread_num() == 0);
}

static void test_thread_capacity_is_one() {
    CHECK(omp_get_max_threads() == 1);
    CHECK(omp_get_num_procs() == 1);

    // Benchmarks may ask for more threads; the stub accepts and ignores it.
    omp_set_num_threads(4);
    CHECK(omp_get_max_threads() == 1);
    CHECK(omp_get_num_threads() == 1);
}

int main() {
    RUN_TEST(test_sequential_identity);
    RUN_TEST(test_thread_capacity_is_one);
    return testfw::summary();
}
