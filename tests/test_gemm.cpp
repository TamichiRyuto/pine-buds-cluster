// Host-side TDD tests for the GEMM kernel.
//
// Test list (t-wada style):
// [x] 1x1: gemm computes c[0] = a[0] * b[0]
// [x] 2x2: general accumulation over k
// [x] rectangular M,N,K: non-square shapes work
// [x] row range [m0, m1): rows outside the range are left untouched
// [x] selftest: A=B=1, N=32 => checksum == 32768.0f exactly, PASS
// [ ] partition equivalence: two half-range calls == one full-range call

#include "test_framework.h"

#include "gemm.h"
#include "gemm_selftest.h"

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

static void test_rectangular() {
    // A is 2x3, B is 3x1 -> C is 2x1
    const float a[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const float b[3] = {1.0f, 10.0f, 100.0f};
    float c[2] = {};
    gemm(0, 2, 1, 3, a, b, c);
    CHECK_EQ_F(c[0], 321.0f);
    CHECK_EQ_F(c[1], 654.0f);
}

static void test_row_range_leaves_other_rows_untouched() {
    const float a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    const float b[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    float c[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    gemm(1, 2, 2, 2, a, b, c);  // compute row 1 only
    CHECK_EQ_F(c[0], -1.0f);    // row 0 untouched
    CHECK_EQ_F(c[1], -1.0f);
    CHECK_EQ_F(c[2], 43.0f);
    CHECK_EQ_F(c[3], 50.0f);
}

static void test_selftest_ones_matrix_passes() {
    const GemmSelftestResult r = gemm_selftest();
    CHECK(r.pass);
    CHECK_EQ_F(r.checksum, 32768.0f);  // N^3 for N=32, exact in float32
    CHECK_EQ_F(r.expect, 32768.0f);
    CHECK(r.fail_i == -1);
    CHECK(r.fail_j == -1);
}

int main() {
    RUN_TEST(test_1x1);
    RUN_TEST(test_2x2);
    RUN_TEST(test_rectangular);
    RUN_TEST(test_row_range_leaves_other_rows_untouched);
    RUN_TEST(test_selftest_ones_matrix_passes);
    return testfw::summary();
}
