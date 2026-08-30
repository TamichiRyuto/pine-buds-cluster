// Benchmark-shaped reference GEMM: first test subject of the
// unmodified-benchmark strategy.
//
// This file is written against the standard MPI + OpenMP APIs and is meant
// to build unmodified in two different worlds:
//   - against our adapters (adapters/mpi, adapters/omp) for host tests and,
//     eventually, target firmware;
//   - against a real OpenMPI + libgomp toolchain on a host, as a golden
//     reference for what a genuine HPC mini-benchmark looks like.
// The only thing that changes between the two is which headers/libraries
// the -I/-L flags resolve <mpi.h> and <omp.h> to; this source never branches
// on which one it got.
//
// Verification rule (precision-independent): A and B are filled with all
// ones, so every element of C is exactly N (sum of N products of 1.0f), and
// the global checksum (sum of all of C, reduced across ranks) is exactly
// N^3, representable exactly in float32 for the N this benchmark targets.

#include <mpi.h>
#include <omp.h>

#define GEMM_BENCH_MAX_N 32

static float g_a[GEMM_BENCH_MAX_N * GEMM_BENCH_MAX_N];
static float g_b[GEMM_BENCH_MAX_N * GEMM_BENCH_MAX_N];
static float g_c[GEMM_BENCH_MAX_N * GEMM_BENCH_MAX_N];

extern "C" int gemm_bench_run(int n, float *checksum_out) {
    if (n <= 0 || n > GEMM_BENCH_MAX_N) {
        return 1;
    }

    int rank = 0;
    int size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    // Every rank fills the full A and B with identical values (1.0f), so
    // concurrent in-process writes from other ranks (host pthread harness)
    // are benign: all writers store the same value to the same addresses.
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            g_a[i * n + j] = 1.0f;
            g_b[i * n + j] = 1.0f;
        }
    }

    int m0 = rank * n / size;
    int m1 = (rank + 1) * n / size;

    // Row-disjoint writes: this rank only ever writes C rows [m0, m1), so
    // other ranks concurrently writing their own disjoint row ranges never
    // race with this one. Inner loop variables are declared in-loop so a
    // real -fopenmp build makes them thread-private, not shared.
    #pragma omp parallel for
    for (int i = m0; i < m1; ++i) {
        for (int j = 0; j < n; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < n; ++k) {
                sum += g_a[i * n + k] * g_b[k * n + j];
            }
            g_c[i * n + j] = sum;
        }
    }

    float local_checksum = 0.0f;
    for (int i = m0; i < m1; ++i) {
        for (int j = 0; j < n; ++j) {
            local_checksum += g_c[i * n + j];
        }
    }

    int rc = MPI_Allreduce(&local_checksum, checksum_out, 1, MPI_FLOAT,
                            MPI_SUM, MPI_COMM_WORLD);
    if (rc != MPI_SUCCESS) {
        return rc;
    }
    return 0;
}

#ifndef GEMM_BENCH_NO_MAIN
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);

    double t0 = MPI_Wtime();
    float checksum = 0.0f;
    int rc = gemm_bench_run(32, &checksum);
    double t1 = MPI_Wtime();

    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    float expected = 32768.0f; /* 32^3 */
    int pass = (rc == 0) && (checksum == expected);

    if (rank == 0) {
        printf("checksum=%f expected=%f elapsed_s=%f %s\n",
               (double)checksum, (double)expected, t1 - t0,
               pass ? "PASS" : "FAIL");
    }

    MPI_Finalize();
    return pass ? 0 : 1;
}
#endif /* GEMM_BENCH_NO_MAIN */
