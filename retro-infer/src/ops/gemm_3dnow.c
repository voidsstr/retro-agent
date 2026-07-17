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

/* NT form (dot products, 2-wide with pfacc horizontal sum) — used by the
 * training forward pass; keeps the Athlon off the scalar fallback. */
void gemm_f32_nt_3dnow(int M, int N, int K, const float *A, const float *B,
                       float *C, int beta0);
void gemm_f32_nt_3dnow(int M, int N, int K, const float *A, const float *B,
                       float *C, int beta0)
{
    int i, j, k;
    int K2 = K & ~1;
    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(float));
    for (i = 0; i < M; i++) {
        const float *Ai = A + (size_t)i * K;
        for (j = 0; j < N; j++) {
            const float *Bj = B + (size_t)j * K;
            __m64 acc = _mm_setzero_si64();
            int bits;
            float accs;
            for (k = 0; k < K2; k += 2)
                acc = _m_pfadd(acc, _m_pfmul(*(const __m64 *)&Ai[k],
                                             *(const __m64 *)&Bj[k]));
            /* horizontal sum, extract as INT bits (no x87 while MMX live) */
            bits = _mm_cvtsi64_si32(_m_pfacc(acc, acc));
            _m_femms();
            memcpy(&accs, &bits, 4);
            for (; k < K; k++)
                accs += Ai[k] * Bj[k];
            C[i * N + j] += accs;
        }
    }
    _m_femms();
}

/* TN form (axpy, 2-wide over j) — training weight-gradient pass */
void gemm_f32_tn_3dnow(int M, int N, int K, const float *A, const float *B,
                       float *C, int beta0);
void gemm_f32_tn_3dnow(int M, int N, int K, const float *A, const float *B,
                       float *C, int beta0)
{
    int i, j, k;
    int N2 = N & ~1;
    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(float));
    for (k = 0; k < K; k++) {
        const float *Ak = A + (size_t)k * M;
        const float *Bk = B + (size_t)k * N;
        for (i = 0; i < M; i++) {
            __m64 a2;
            float *Ci = C + (size_t)i * N;
            /* broadcast via integer load — no x87 while MMX is live */
            a2 = _mm_cvtsi32_si64(*(const int *)&Ak[i]);
            a2 = _mm_unpacklo_pi32(a2, a2);
            for (j = 0; j < N2; j += 2)
                *(__m64 *)&Ci[j] = _m_pfadd(*(__m64 *)&Ci[j],
                                            _m_pfmul(a2, *(const __m64 *)&Bk[j]));
        }
        if (N & 1) {
            _m_femms();
            for (i = 0; i < M; i++)
                C[(size_t)i * N + N - 1] += Ak[i] * Bk[N - 1];
        }
    }
    _m_femms();
}
