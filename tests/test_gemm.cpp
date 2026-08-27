// Host-side TDD tests for the GEMM kernel.
//
// Test list (t-wada style):
// [ ] 1x1: gemm computes c[0] = a[0] * b[0]
// [ ] 2x2: general accumulation over k
// [ ] rectangular M,N,K: non-square shapes work
// [ ] row range [m0, m1): rows outside the range are left untouched
// [ ] selftest: A=B=1, N=32 => checksum == 32768.0f exactly, PASS
// [ ] partition equivalence: two half-range calls == one full-range call

#include "test_framework.h"

int main() {
    return testfw::summary();
}
