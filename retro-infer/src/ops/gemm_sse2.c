/*
 * SSE2 int8 GEMM (i32 accumulate). This TU is the only code compiled with
 * -msse2; only reached when cpu_caps_t.sse2 is set (the Athlon 64 / P4 /
 * Core-class fleet boxes). 128-bit = 8 output columns per k-step, double the
 * MMX path's 4. Integer math is exact, so bit-identical to gemm_i8_scalar
 * regardless of order.
 *
 * Per (i, 8 columns): for each k, broadcast A[i,k] to 8 i16 lanes; load 8
 * bytes of B row k, sign-extend to 8 i16; zero-interleave into [b,0,...]
 * halves so pmaddwd yields (a*b + a*0) = a*b per i32 lane; paddd accumulate
 * into two 4-lane i32 registers (columns 0-3 and 4-7).
 */
#include <emmintrin.h>
#include <string.h>
#include "../infer.h"

void gemm_i8_sse2(int M, int N, int K, const signed char *A,
                  const signed char *B, int *C, int beta0);

void gemm_i8_sse2(int M, int N, int K, const signed char *A,
                  const signed char *B, int *C, int beta0)
{
    int i, j, k;
    int N8 = N & ~7;
    __m128i zero = _mm_setzero_si128();

    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(int));

    for (i = 0; i < M; i++) {
        const signed char *Ai = A + (size_t)i * K;
        int *Ci = C + (size_t)i * N;
        for (j = 0; j < N8; j += 8) {
            __m128i acc03 = _mm_loadu_si128((const __m128i *)&Ci[j]);
            __m128i acc47 = _mm_loadu_si128((const __m128i *)&Ci[j + 4]);
            for (k = 0; k < K; k++) {
                __m128i a16 = _mm_set1_epi16((short)Ai[k]);
                /* 8 signed bytes of B row k, cols j..j+7 -> 8 i16 */
                __m128i b8 = _mm_loadl_epi64(
                    (const __m128i *)(B + (size_t)k * N + j));
                __m128i b16 = _mm_srai_epi16(_mm_unpacklo_epi8(b8, b8), 8);
                __m128i blo = _mm_unpacklo_epi16(b16, zero); /* [b0,0,b1,0,..] */
                __m128i bhi = _mm_unpackhi_epi16(b16, zero);
                acc03 = _mm_add_epi32(acc03, _mm_madd_epi16(a16, blo));
                acc47 = _mm_add_epi32(acc47, _mm_madd_epi16(a16, bhi));
            }
            _mm_storeu_si128((__m128i *)&Ci[j], acc03);
            _mm_storeu_si128((__m128i *)&Ci[j + 4], acc47);
        }
        for (; j < N; j++) {
            int acc = Ci[j];
            for (k = 0; k < K; k++)
                acc += (int)Ai[k] * (int)B[(size_t)k * N + j];
            Ci[j] = acc;
        }
    }
}
