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
