// GEMM kernel for PineBuds Pro (BES2300YP, Cortex-M4F).
// Freestanding C++: no STL, no heap, float only. Pure function: no globals,
// no I/O; logging is the caller's job.
#ifndef GEMM_H
#define GEMM_H

// C = A * B for rows [m0, m1) only, so future core/bud partitioning can
// split the M dimension. A is m x k, B is k x n, C is m x n (row-major).
void gemm(int m0, int m1, int n, int k, const float* a, const float* b, float* c);

#endif  // GEMM_H
