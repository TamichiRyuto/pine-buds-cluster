// Host-side TDD tests for the GEMM kernel.
//
// Test list (t-wada style):
// [x] 1x1: gemm computes c[0] = a[0] * b[0]
// [x] 2x2: general accumulation over k
// [ ] rectangular M,N,K: non-square shapes work
// [ ] row range [m0, m1): rows outside the range are left untouched
// [ ] selftest: A=B=1, N=32 => checksum == 32768.0f exactly, PASS
// [ ] partition equivalence: two half-range calls == one full-range call

#include "test_framework.h"

#include "gemm.h"

static void test_1x1() {
    const float a[1] = {3.0f};
    const float b[1] = {4.0f};
    float c[1] = {0.0f};
    gemm(0, 1, 1, 1, a, b, c);
    CHECK_EQ_F(c[0], 12.0f);
}

static void test_2x2() {
    // [1 2; 3 4] * [5 6; 7 8] = [19 22; 43 50]
    const float a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    const float b[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    float c[4] = {};
    gemm(0, 2, 2, 2, a, b, c);
    CHECK_EQ_F(c[0], 19.0f);
    CHECK_EQ_F(c[1], 22.0f);
    CHECK_EQ_F(c[2], 43.0f);
    CHECK_EQ_F(c[3], 50.0f);
}

int main() {
    RUN_TEST(test_1x1);
    RUN_TEST(test_2x2);
    return testfw::summary();
}
