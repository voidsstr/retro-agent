/*
 * Scalar reference GEMM kernels — the parity oracle for every vectorized and
 * GPU path. Accumulation order is FIXED (k innermost, ascending) so results
 * are bit-for-bit reproducible across machines; never reorder these loops.
 */
#include <string.h>
#include "../infer.h"

void gemm_f32_scalar(int M, int N, int K, const float *A, const float *B,
                     float *C, int beta0)
{
    int i, j, k;
    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(float));
    for (i = 0; i < M; i++) {
        for (j = 0; j < N; j++) {
            float acc = C[i * N + j];
            for (k = 0; k < K; k++)
                acc += A[i * K + k] * B[k * N + j];
            C[i * N + j] = acc;
        }
    }
}

void gemm_i8_scalar(int M, int N, int K, const signed char *A,
                    const signed char *B, int *C, int beta0)
{
    int i, j, k;
    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(int));
    for (i = 0; i < M; i++) {
        for (j = 0; j < N; j++) {
            int acc = C[i * N + j];
            for (k = 0; k < K; k++)
                acc += (int)A[i * K + k] * (int)B[k * N + j];
            C[i * N + j] = acc;
        }
    }
}

/* C[M,N] += A[M,K] * B^T, where B is [N,K] row-major (dot-product form) */
void gemm_f32_nt_scalar(int M, int N, int K, const float *A, const float *B,
                        float *C, int beta0)
{
    int i, j, k;
    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(float));
    for (i = 0; i < M; i++) {
        for (j = 0; j < N; j++) {
            float acc = C[i * N + j];
            const float *Ai = A + (size_t)i * K;
            const float *Bj = B + (size_t)j * K;
            for (k = 0; k < K; k++)
                acc += Ai[k] * Bj[k];
            C[i * N + j] = acc;
        }
    }
}

/* C[M,N] += A^T * B, where A is [K,M], B is [K,N] (both row-major) */
void gemm_f32_tn_scalar(int M, int N, int K, const float *A, const float *B,
                        float *C, int beta0)
{
    int i, j, k;
    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(float));
    for (k = 0; k < K; k++) {
        const float *Ak = A + (size_t)k * M;
        const float *Bk = B + (size_t)k * N;
        for (i = 0; i < M; i++) {
            float a = Ak[i];
            float *Ci = C + (size_t)i * N;
            for (j = 0; j < N; j++)
                Ci[j] += a * Bk[j];
        }
    }
}
