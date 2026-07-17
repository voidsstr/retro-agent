/*
 * Runtime kernel dispatch. The binary targets i586; vectorized TUs are only
 * referenced here behind capability checks so a K6-2 never executes them.
 */
#include "infer.h"

/* From ops/gemm_sse.c (compiled -msse) */
void gemm_f32_sse(int M, int N, int K, const float *A, const float *B,
                  float *C, int beta0);

kernels_t g_kernels;

void kernels_init(const cpu_caps_t *caps)
{
    g_kernels.gemm_f32 = gemm_f32_scalar;
    g_kernels.gemm_f32_name = "scalar";
    g_kernels.gemm_i8 = gemm_i8_scalar;
    g_kernels.gemm_i8_name = "scalar";

    if (caps->sse) {
        g_kernels.gemm_f32 = gemm_f32_sse;
        g_kernels.gemm_f32_name = "sse";
    }
}
