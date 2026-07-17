/*
 * 3DNow! fp32 GEMM for the K6-2/Athlon boxes (no SSE). This TU is the only
 * code compiled with -m3dnow; only reached when cpu_caps_t.amd3dnow is set.
 *
 * 2-wide over output columns; per-element accumulation is k-ascending with
 * single-precision rounding each step, so results match the SSE path closely
 * but NOT bit-for-bit vs the x87 scalar path (which keeps excess precision).
 * Parity tests use fp tolerance for f32 kernels; int8 paths are exact.
 */
#include <mm3dnow.h>
#include <string.h>
#include "../infer.h"

void gemm_f32_3dnow(int M, int N, int K, const float *A, const float *B,
                    float *C, int beta0);

void gemm_f32_3dnow(int M, int N, int K, const float *A, const float *B,
                    float *C, int beta0)
{
    int i, j, k;
    int N2 = N & ~1;

    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(float));

    for (i = 0; i < M; i++) {
        const float *Ai = A + (size_t)i * K;
        float *Ci = C + (size_t)i * N;
        for (j = 0; j < N2; j += 2) {
            __m64 acc = *(const __m64 *)&Ci[j];
            for (k = 0; k < K; k++) {
                __m64 a2, b2;
                a2 = _mm_cvtsi32_si64(*(const int *)&Ai[k]);
                a2 = _mm_unpacklo_pi32(a2, a2);          /* [a, a] */
                b2 = *(const __m64 *)(B + (size_t)k * N + j);
                acc = _m_pfadd(acc, _m_pfmul(a2, b2));
            }
            *(__m64 *)&Ci[j] = acc;
        }
        if (j < N) {
            /* last odd column via x87 scalar — femms BEFORE any FPU op */
            float acc;
            _m_femms();
            acc = Ci[j];
            for (k = 0; k < K; k++)
                acc += Ai[k] * B[(size_t)k * N + j];
            Ci[j] = acc;
        }
    }
    _m_femms();
}
