/*
 * Runtime kernel dispatch. The binary targets i586; vectorized TUs are only
 * referenced here behind capability checks so a plain Pentium never executes
 * them. f32 preference: SSE > 3DNow! > scalar. i8: MMX > scalar (exact
 * integer math either way).
 */
#include "infer.h"

/* ops/gemm_sse.c (-msse) */
void gemm_f32_sse(int M, int N, int K, const float *A, const float *B,
                  float *C, int beta0);
void gemm_f32_nt_sse(int M, int N, int K, const float *A, const float *B,
                     float *C, int beta0);
void gemm_f32_tn_sse(int M, int N, int K, const float *A, const float *B,
                     float *C, int beta0);
#ifndef RI_NO_3DNOW
/* ops/gemm_3dnow.c (-m3dnow) */
void gemm_f32_3dnow(int M, int N, int K, const float *A, const float *B,
                    float *C, int beta0);
#endif
/* ops/gemm_mmx.c (-mmmx) */
void gemm_i8_mmx(int M, int N, int K, const signed char *A,
                 const signed char *B, int *C, int beta0);

kernels_t g_kernels;

void kernels_init(const cpu_caps_t *caps)
{
    g_kernels.gemm_f32 = gemm_f32_scalar;
    g_kernels.gemm_f32_name = "scalar";
    g_kernels.gemm_i8 = gemm_i8_scalar;
    g_kernels.gemm_i8_name = "scalar";
    g_kernels.gemm_f32_nt = gemm_f32_nt_scalar;
    g_kernels.gemm_f32_tn = gemm_f32_tn_scalar;

    if (caps->sse) {
        g_kernels.gemm_f32 = gemm_f32_sse;
        g_kernels.gemm_f32_name = "sse";
        g_kernels.gemm_f32_nt = gemm_f32_nt_sse;
        g_kernels.gemm_f32_tn = gemm_f32_tn_sse;
    }
#ifndef RI_NO_3DNOW
    else if (caps->amd3dnow) {
        g_kernels.gemm_f32 = gemm_f32_3dnow;
        g_kernels.gemm_f32_name = "3dnow";
    }
#endif
    if (caps->mmx) {
        g_kernels.gemm_i8 = gemm_i8_mmx;
        g_kernels.gemm_i8_name = "mmx";
    }
}

void kernels_force_scalar(void)
{
    g_kernels.gemm_f32 = gemm_f32_scalar;
    g_kernels.gemm_f32_name = "scalar";
    g_kernels.gemm_i8 = gemm_i8_scalar;
    g_kernels.gemm_i8_name = "scalar";
    g_kernels.gemm_f32_nt = gemm_f32_nt_scalar;
    g_kernels.gemm_f32_tn = gemm_f32_tn_scalar;
}
