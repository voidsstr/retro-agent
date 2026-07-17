/*
 * GFLOP/s microbench for --selfcheck and the AI_HELLO capability estimate.
 * Uses whatever gemm_f32 the dispatcher selected.
 */
#include <stdlib.h>
#include "infer.h"
#include "port.h"

double bench_gemm_gflops(int size, int iters)
{
    double t0, t1;
    float *A, *B, *C;
    size_t n = (size_t)size * size;
    double secs, flops;
    int i;

    A = (float *)malloc(n * sizeof(float));
    B = (float *)malloc(n * sizeof(float));
    C = (float *)malloc(n * sizeof(float));
    if (!A || !B || !C) {
        free(A); free(B); free(C);
        return -1.0;
    }
    /* Deterministic fill; values ~[-1,1] to stay in fp range */
    for (i = 0; i < (int)n; i++) {
        A[i] = (float)((i * 2654435761u >> 16) & 0xFFF) / 2048.0f - 1.0f;
        B[i] = (float)((i * 40503u >> 8) & 0xFFF) / 2048.0f - 1.0f;
    }

    /* Warmup pass so the first-touch page faults don't count */
    g_kernels.gemm_f32(size, size, size, A, B, C, 1);

    t0 = ri_now();
    for (i = 0; i < iters; i++)
        g_kernels.gemm_f32(size, size, size, A, B, C, 1);
    t1 = ri_now();

    secs = t1 - t0;
    flops = 2.0 * (double)size * size * size * iters;
    free(A); free(B); free(C);
    if (secs <= 0.0)
        return -1.0;
    return flops / secs / 1e9;
}
