/*
 * retro-infer - dependency-free ML runtime for the retro fleet (Win98/2K/XP, i586+)
 *
 * Core types shared by the CLI, kernels, and (later) the GPU backends.
 * No CRT beyond msvcrt basics; no floats in structs that cross the wire
 * without explicit dtype tags.
 */
#ifndef RETRO_INFER_H
#define RETRO_INFER_H

#include <stddef.h>

#ifndef INFER_VERSION
#define INFER_VERSION "0.0.0-dev"
#endif

/* ---- CPU capabilities (cpuid.c) ---- */
typedef struct {
    int has_cpuid;
    int mmx;
    int sse;
    int sse2;
    int amd3dnow;
    int amd3dnowext;
    char vendor[13];     /* "GenuineIntel" / "AuthenticAMD" */
    char brand[49];      /* CPUID brand string, may be empty on old CPUs */
    unsigned family, model, stepping;
} cpu_caps_t;

void cpu_detect(cpu_caps_t *caps);

/* ---- Tensors ---- */
typedef enum {
    DT_F32 = 0,
    DT_I8  = 1,
    DT_U8  = 2,
    DT_I32 = 3,
    DT_BIN = 4          /* 1 bit/weight, packed, for BNN/XNOR nets */
} dtype_t;

#define MAX_DIMS 4

typedef struct {
    dtype_t dtype;
    int ndim;
    int shape[MAX_DIMS];   /* row-major; unused dims = 1 */
    void *data;
    float scale;           /* int8 quant: real = scale * (q - zero_point) */
    int zero_point;
} tensor_t;

size_t dtype_size(dtype_t dt);
size_t tensor_elems(const tensor_t *t);
size_t tensor_bytes(const tensor_t *t);

/* ---- Kernel dispatch (populated by kernels_init from cpu caps) ---- */
typedef struct {
    /* C[M,N] += A[M,K] * B[K,N], fp32 row-major. beta0: C zeroed first */
    void (*gemm_f32)(int M, int N, int K, const float *A, const float *B,
                     float *C, int beta0);
    /* int8 GEMM with int32 accumulate: C[M,N](i32) = A[M,K](i8) * B[K,N](i8) */
    void (*gemm_i8)(int M, int N, int K, const signed char *A,
                    const signed char *B, int *C, int beta0);
    /* training variants: NT (B is [N,K]) and TN (A is [K,M], B is [K,N]) */
    void (*gemm_f32_nt)(int M, int N, int K, const float *A, const float *B,
                        float *C, int beta0);
    void (*gemm_f32_tn)(int M, int N, int K, const float *A, const float *B,
                        float *C, int beta0);
    const char *gemm_f32_name;   /* "scalar" / "sse" / "3dnow" */
    const char *gemm_i8_name;    /* "scalar" / "mmx" */
} kernels_t;

void gemm_f32_nt_scalar(int M, int N, int K, const float *A, const float *B,
                        float *C, int beta0);
void gemm_f32_tn_scalar(int M, int N, int K, const float *A, const float *B,
                        float *C, int beta0);

extern kernels_t g_kernels;
void kernels_init(const cpu_caps_t *caps);
void kernels_force_scalar(void);

/* Reference scalar kernels (always available; parity oracle) */
void gemm_f32_scalar(int M, int N, int K, const float *A, const float *B,
                     float *C, int beta0);
void gemm_i8_scalar(int M, int N, int K, const signed char *A,
                    const signed char *B, int *C, int beta0);

/* ---- Microbench (bench.c) ---- */
/* Returns achieved GFLOP/s for an fp32 GEMM using the dispatched kernel. */
double bench_gemm_gflops(int size, int iters);

/* ---- .rim model container (rim.c) ---- */
typedef struct {
    char *manifest_json;       /* heap-owned, NUL-terminated */
    size_t manifest_len;
    unsigned char *weights;    /* heap-owned blob, 16-byte aligned base */
    size_t weights_len;
} rim_model_t;

/* Returns 0 on success, nonzero + message in errbuf on failure. */
int rim_load(const char *path, rim_model_t *out, char *errbuf, size_t errlen);
void rim_free(rim_model_t *m);

#endif /* RETRO_INFER_H */
