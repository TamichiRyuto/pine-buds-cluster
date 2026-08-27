#include "gemm.h"

void gemm(int m0, int m1, int n, int k, const float* a, const float* b, float* c) {
    for (int i = m0; i < m1; ++i) {
        for (int j = 0; j < n; ++j) {
            float acc = 0.0f;
            for (int p = 0; p < k; ++p) {
                acc += a[i * k + p] * b[p * n + j];
            }
            c[i * n + j] = acc;
        }
    }
}
