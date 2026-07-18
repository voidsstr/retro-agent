/*
 * --glide-check: M5 acceptance driver. Runs on the Voodoo box via the agent:
 *   1. CPU reference bgemm (always)
 *   2. glide_init + hardware bgemm
 *   3. exact-match compare + max abs error in "steps" (match counts)
 *   4. FNV-1a hash of the GPU result (stability guard across runs)
 *   5. throughput (binary MACs/s) GPU vs CPU
 * Prints key=value lines like every other retro-infer mode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../infer.h"
#include "../port.h"
#include "../ops/nn.h"
#include "glide_mac.h"
#include "nv_gl.h"

/* Backend vtable so one acceptance driver serves both GPU backends. */
typedef struct {
    const char *tag;      /* output prefix, e.g. "glide" / "nvgl" */
    int (*available)(void);
    int (*init)(char *, size_t);
    void (*shutdown)(void);
    int (*bgemm)(int, int, int, const unsigned char *, const unsigned char *,
                 int *, char *, size_t);
} gpu_backend_t;

static const gpu_backend_t GB_GLIDE = {
    "glide", glide_available, glide_init, glide_shutdown, glide_bgemm};
static const gpu_backend_t GB_NVGL = {
    "nvgl", nvgl_available, nvgl_init, nvgl_shutdown, nvgl_bgemm};

void bgemm_cpu(int M, int N, int K, const unsigned char *A,
               const unsigned char *B, int *C_matches)
{
    int i, j, k;
    for (i = 0; i < M; i++) {
        for (j = 0; j < N; j++) {
            int m = 0;
            for (k = 0; k < K; k++)
                if (A[i * K + k] == B[k * N + j])
                    m++;
            C_matches[i * N + j] += m;
        }
    }
}

static unsigned fnv1a(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned h = 2166136261u;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static int gpu_check(const gpu_backend_t *gb, int M, int N, int K,
                     unsigned seed)
{
    unsigned char *A, *B;
    int *Cg, *Cc;
    char err[128] = "";
    unsigned s = seed ? seed : 42;
    int i, rc, mismatches = 0, max_abs = 0;
    double t0, t_gpu, t_cpu;

    printf("%s.available=%d\n", gb->tag, gb->available());
    A = (unsigned char *)malloc((size_t)M * K);
    B = (unsigned char *)malloc((size_t)K * N);
    Cg = (int *)calloc((size_t)M * N, sizeof(int));
    Cc = (int *)calloc((size_t)M * N, sizeof(int));
    if (!A || !B || !Cg || !Cc) {
        printf("%s-check: FAIL: oom\n", gb->tag);
        return 1;
    }
    for (i = 0; i < M * K; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        A[i] = s & 1;
    }
    for (i = 0; i < K * N; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        B[i] = s & 1;
    }

    t0 = ri_now();
    bgemm_cpu(M, N, K, A, B, Cc);
    t_cpu = ri_now() - t0;

    /* bit-packed CPU reference (the honest speed comparator) */
    {
        int kb = (K + 7) / 8;
        unsigned char *Ap = (unsigned char *)calloc((size_t)M * kb, 1);
        unsigned char *Bp = (unsigned char *)calloc((size_t)N * kb, 1);
        int *Cp = (int *)calloc((size_t)M * N, sizeof(int));
        double t_pack;
        if (Ap && Bp && Cp) {
            int ii, jj, kk2, bad = 0;
            for (ii = 0; ii < M; ii++)
                for (kk2 = 0; kk2 < K; kk2++)
                    if (A[ii * K + kk2])
                        Ap[ii * kb + (kk2 >> 3)] |=
                            (unsigned char)(1 << (kk2 & 7));
            for (jj = 0; jj < N; jj++)
                for (kk2 = 0; kk2 < K; kk2++)
                    if (B[kk2 * N + jj])
                        Bp[jj * kb + (kk2 >> 3)] |=
                            (unsigned char)(1 << (kk2 & 7));
            t0 = ri_now();
            for (ii = 0; ii < M; ii++)
                for (jj = 0; jj < N; jj++)
                    Cp[ii * N + jj] =
                        (int)bnn_xnor_matches(Ap + (size_t)ii * kb,
                                              Bp + (size_t)jj * kb, K)
                        - (K % 8 ? (8 - K % 8) : 0);
            t_pack = ri_now() - t0;
            for (ii = 0; ii < M * N && K % 8 == 0; ii++)
                if (Cp[ii] != Cc[ii])
                    bad++;
            printf("%s.cpu_bitpacked_mmacs=%.2f (parity_bad=%d)\n", gb->tag,
                   (double)M * N * K / t_pack / 1e6, bad);
        }
        free(Ap); free(Bp); free(Cp);
    }

    if (gb->init(err, sizeof(err)) != 0) {
        printf("%s.init=FAIL %s\n", gb->tag, err);
        printf("%s-check: FAIL\n", gb->tag);
        free(A); free(B); free(Cg); free(Cc);
        return 1;
    }
    printf("%s.init=OK\n", gb->tag);
    fflush(stdout);

    t0 = ri_now();
    rc = gb->bgemm(M, N, K, A, B, Cg, err, sizeof(err));
    t_gpu = ri_now() - t0;
    if (rc != 0) {
        printf("%s.bgemm=FAIL %s\n", gb->tag, err);
        gb->shutdown();
        free(A); free(B); free(Cg); free(Cc);
        printf("%s-check: FAIL\n", gb->tag);
        return 1;
    }

    for (i = 0; i < M * N; i++) {
        int d = Cg[i] - Cc[i];
        if (d < 0)
            d = -d;
        if (d) {
            mismatches++;
            if (d > max_abs)
                max_abs = d;
        }
    }
    printf("%s.check.m=%d n=%d k=%d seed=%u\n", gb->tag, M, N, K, seed);
    printf("%s.mismatches=%d %s.max_abs_err_steps=%d\n", gb->tag, gb->tag, mismatches,
           max_abs);
    printf("%s.result_hash=%08x\n", gb->tag, fnv1a(Cg, (size_t)M * N * sizeof(int)));
    printf("%s.gpu_secs=%.4f %s.cpu_secs=%.4f\n", gb->tag, gb->tag, t_gpu, t_cpu);
    printf("%s.gpu_mmacs=%.2f %s.cpu_mmacs=%.2f\n", gb->tag, gb->tag,
           (double)M * N * K / t_gpu / 1e6, (double)M * N * K / t_cpu / 1e6);

    /* second run for intra-process stability */
    memset(Cg, 0, (size_t)M * N * sizeof(int));
    rc = gb->bgemm(M, N, K, A, B, Cg, err, sizeof(err));
    printf("%s.rerun_hash=%08x\n", gb->tag,
           rc == 0 ? fnv1a(Cg, (size_t)M * N * sizeof(int)) : 0);

    gb->shutdown();
    free(A); free(B); free(Cg); free(Cc);
    printf("%s-check: %s\n", gb->tag, mismatches == 0 ? "OK" : "MISMATCH");
    return mismatches != 0;
}

int glide_check(int M, int N, int K, unsigned seed)
{
    return gpu_check(&GB_GLIDE, M, N, K, seed);
}

int nv_check(int M, int N, int K, unsigned seed)
{
    return gpu_check(&GB_NVGL, M, N, K, seed);
}
