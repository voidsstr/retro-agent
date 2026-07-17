/*
 * MMX int8 GEMM (i32 accumulate). This TU is the only code compiled with
 * -mmmx; only reached when cpu_caps_t.mmx is set. Integer math is exact, so
 * results are bit-identical to gemm_i8_scalar regardless of order.
 *
 * Per (i, 4 columns): for each k, broadcast A[i,k] to 4 i16 lanes, sign-extend
 * 4 bytes of B row k, zero-interleave to two [b,0,b,0] pairs, pmaddwd gives
 * (a*b0, a*b1) / (a*b2, a*b3) as i32 lanes, paddd accumulates.
 */
#include <mmintrin.h>
#include <string.h>
#include "../infer.h"

void gemm_i8_mmx(int M, int N, int K, const signed char *A,
                 const signed char *B, int *C, int beta0);

void gemm_i8_mmx(int M, int N, int K, const signed char *A,
                 const signed char *B, int *C, int beta0)
{
    int i, j, k;
    int N4 = N & ~3;
    __m64 zero = _mm_setzero_si64();

    if (beta0)
        memset(C, 0, (size_t)M * N * sizeof(int));

    for (i = 0; i < M; i++) {
        const signed char *Ai = A + (size_t)i * K;
        int *Ci = C + (size_t)i * N;
        for (j = 0; j < N4; j += 4) {
            __m64 acc01 = *(__m64 *)&Ci[j];
            __m64 acc23 = *(__m64 *)&Ci[j + 2];
            for (k = 0; k < K; k++) {
                short av = (short)Ai[k];
                __m64 a16 = _mm_set1_pi16(av);
                /* 4 bytes of B row k, columns j..j+3 */
                __m64 b8 = _mm_cvtsi32_si64(*(const int *)(B + (size_t)k * N + j));
                __m64 b16 = _mm_srai_pi16(_mm_unpacklo_pi8(b8, b8), 8);
                __m64 b01 = _mm_unpacklo_pi16(b16, zero); /* [b0,sgn?,b1,..] */
                __m64 b23 = _mm_unpackhi_pi16(b16, zero);
                /* zero-interleave keeps high word 0, but sign matters:
                 * pmaddwd treats words as SIGNED; the zero word contributes
                 * 0 regardless, and b words are already sign-extended i16.
                 * (a*b) fits i32; pair-sum adds a*b + a*0. */
                acc01 = _mm_add_pi32(acc01, _mm_madd_pi16(a16, b01));
                acc23 = _mm_add_pi32(acc23, _mm_madd_pi16(a16, b23));
            }
            *(__m64 *)&Ci[j] = acc01;
            *(__m64 *)&Ci[j + 2] = acc23;
        }
        for (; j < N; j++) {
            int acc = Ci[j];
            for (k = 0; k < K; k++)
                acc += (int)Ai[k] * (int)B[(size_t)k * N + j];
            Ci[j] = acc;
        }
    }
    _mm_empty();
}
