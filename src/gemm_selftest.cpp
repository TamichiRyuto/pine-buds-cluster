#include "gemm_selftest.h"

#include "gemm.h"

namespace {
const int N = kGemmSelftestN;
// Static buffers: 3 * 32*32*4 = 12 KB, fits the SRAM budget. No heap.
float A[N][N];
float B[N][N];
float C[N][N];
}  // namespace

GemmSelftestResult gemm_selftest() {
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            A[i][j] = 1.0f;
            B[i][j] = 1.0f;
            C[i][j] = 0.0f;
        }
    }

    gemm(0, N, N, N, &A[0][0], &B[0][0], &C[0][0]);

    GemmSelftestResult r;
    r.expect = static_cast<float>(N) * static_cast<float>(N) * static_cast<float>(N);
    r.checksum = 0.0f;
    r.fail_i = -1;
    r.fail_j = -1;
    r.fail_value = 0.0f;

    const float expect_elem = static_cast<float>(N);
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            r.checksum += C[i][j];
            if (r.fail_i == -1 && C[i][j] != expect_elem) {
                r.fail_i = i;
                r.fail_j = j;
                r.fail_value = C[i][j];
            }
        }
    }

    r.pass = (r.checksum == r.expect) && (r.fail_i == -1);
    return r;
}
