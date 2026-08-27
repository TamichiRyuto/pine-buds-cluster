// Deterministic, precision-independent GEMM self-test.
// Pure logic shared between host tests and firmware; no I/O here —
// the caller formats/prints the result (UART TRACE on target).
#ifndef GEMM_SELFTEST_H
#define GEMM_SELFTEST_H

inline constexpr int kGemmSelftestN = 32;

struct GemmSelftestResult {
    bool pass;
    float checksum;  // sum of all C elements
    float expect;    // N^3, exactly representable in float32 for N=32
    int fail_i;      // first mismatching element, -1 if none
    int fail_j;
    float fail_value;
};

// A = B = all ones  =>  every C[i][j] == N, checksum == N^3 (integer-exact).
GemmSelftestResult gemm_selftest();

#endif  // GEMM_SELFTEST_H
