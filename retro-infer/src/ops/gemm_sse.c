/*
 * SSE fp32 GEMM. This TU is the ONLY code compiled with -msse (see Makefile);
 * it must not be entered unless cpu_caps_t.sse is set (kernels_init guards).
 *
 * Vectorizes over 4 output columns; per-element accumulation order (k
 * ascending) is identical to gemm_f32_scalar, so results are bit-for-bit
 * equal to the scalar oracle — keep it that way (no reassociation, no FMA).
 */
#include <xmmintrin.h>
#include <string.h>
#include "../infer.h"

void gemm_f32_sse(int M, int N, int K, const float *A, const float *B,
                  float *C, int beta0);

void gemm_f32_sse(int M, int N, int K, const float *A, const float *B,
                  float *C, int beta0)
{
    int i, j, k;
    int N4 = N & ~3;
    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(float));
    for (i = 0; i < M; i++) {
        const float *Ai = A + (size_t)i * K;
        float *Ci = C + (size_t)i * N;
        for (j = 0; j < N4; j += 4) {
            __m128 acc = _mm_loadu_ps(Ci + j);
            for (k = 0; k < K; k++) {
                __m128 a = _mm_set1_ps(Ai[k]);
                __m128 b = _mm_loadu_ps(B + (size_t)k * N + j);
                acc = _mm_add_ps(acc, _mm_mul_ps(a, b));
            }
            _mm_storeu_ps(Ci + j, acc);
        }
        for (; j < N; j++) {
            float acc = Ci[j];
            for (k = 0; k < K; k++)
                acc += Ai[k] * B[(size_t)k * N + j];
            Ci[j] = acc;
        }
    }
}

/* NT form: C[i,j] = dot(A_i, B_j); horizontal sum at the end (fp order
 * differs from scalar — parity tests use tolerance for f32) */
void gemm_f32_nt_sse(int M, int N, int K, const float *A, const float *B,
                     float *C, int beta0);
void gemm_f32_nt_sse(int M, int N, int K, const float *A, const float *B,
                     float *C, int beta0)
{
    int i, j, k;
    int K4 = K & ~3;
    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(float));
    for (i = 0; i < M; i++) {
        const float *Ai = A + (size_t)i * K;
        for (j = 0; j < N; j++) {
            const float *Bj = B + (size_t)j * K;
            __m128 acc4 = _mm_setzero_ps();
            float acc;
            float tmp[4];
            for (k = 0; k < K4; k += 4)
                acc4 = _mm_add_ps(acc4, _mm_mul_ps(_mm_loadu_ps(Ai + k),
                                                   _mm_loadu_ps(Bj + k)));
            _mm_storeu_ps(tmp, acc4);
            acc = tmp[0] + tmp[1] + tmp[2] + tmp[3];
            for (; k < K; k++)
                acc += Ai[k] * Bj[k];
            C[i * N + j] += acc;
        }
    }
}

/* TN form: C[M,N] += A^T B with A [K,M], B [K,N]; axpy pattern, j 4-wide */
void gemm_f32_tn_sse(int M, int N, int K, const float *A, const float *B,
                     float *C, int beta0);
void gemm_f32_tn_sse(int M, int N, int K, const float *A, const float *B,
                     float *C, int beta0)
{
    int i, j, k;
    int N4 = N & ~3;
    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(float));
    for (k = 0; k < K; k++) {
        const float *Ak = A + (size_t)k * M;
        const float *Bk = B + (size_t)k * N;
        for (i = 0; i < M; i++) {
            __m128 a = _mm_set1_ps(Ak[i]);
            float *Ci = C + (size_t)i * N;
            for (j = 0; j < N4; j += 4)
                _mm_storeu_ps(Ci + j,
                              _mm_add_ps(_mm_loadu_ps(Ci + j),
                                         _mm_mul_ps(a, _mm_loadu_ps(Bk + j))));
            for (; j < N; j++)
                Ci[j] += Ak[i] * Bk[j];
        }
    }
}
