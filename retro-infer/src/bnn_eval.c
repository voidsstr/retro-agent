/*
 * --bnn-eval: batched BNN evaluation with a pluggable binary-GEMM backend
 * (M5/M6 model-level acceptance). Runs an all-bdense .rim over an eval set:
 *
 *   cpu   — per-image exec path (bit ops, exact reference)
 *   glide — hidden/output XNOR layers on the Voodoo via glide_bgemm tiles
 *   nvgl  — same, via the portable OpenGL multitexture backend (any GPU
 *           with ARB_multitexture: GeForce, Radeon, Intel integrated)
 *   (layer 1 consumes raw u8 pixels and stays on the CPU either way; the
 *   GPU accelerates the binary layers, batch 256 images per pass)
 *
 * Prints label agreement between the two paths + throughput. The GPU path
 * uses the exact-match-count primitive proven by --glide-check/--nv-check,
 * so labels must agree 100% — anything else is a real bug.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "infer.h"
#include "exec.h"
#include "port.h"
#include "ops/nn.h"
#include "gpu/glide_mac.h"
#include "gpu/nv_gl.h"
#include "bnn_eval.h"

typedef struct {
    const char *tag;
    int (*init)(char *, size_t);
    void (*shutdown)(void);
    int (*bgemm)(int, int, int, const unsigned char *, const unsigned char *,
                 int *, char *, size_t);
} bnn_gpu_t;

static const bnn_gpu_t BNN_GLIDE = {
    "glide", glide_init, glide_shutdown, glide_bgemm};
static const bnn_gpu_t BNN_NVGL = {
    "nvgl", nvgl_init, nvgl_shutdown, nvgl_bgemm};

/* raw manifest access: we re-parse the model as a bdense chain */
#include "json.h"

typedef struct {
    int n_in, n_out, first_u8;
    const unsigned char *w;     /* packed bits [n_out][n_in/8] */
    const int *thresh;          /* NULL = output layer */
} blayer_t;

#define MAX_BL 8
#define GPU_BATCH 256

static int parse_bnn(model_t *m, const char *path, blayer_t *bl, int *n_bl,
                     rim_model_t *rim_out, char *err, size_t errlen)
{
    /* reopen the rim to walk the manifest ourselves */
    jnode_t *root;
    const jnode_t *layers;
    int i, n;
    (void)m;
    if (rim_load(path, rim_out, err, errlen))
        return 1;
    root = json_parse(rim_out->manifest_json);
    if (!root) {
        strncpy(err, "manifest parse", errlen - 1);
        return 1;
    }
    layers = json_get(root, "layers");
    n = json_arr_len(layers);
    if (n <= 0 || n > MAX_BL) {
        strncpy(err, "bad layer count", errlen - 1);
        return 1;
    }
    for (i = 0; i < n; i++) {
        const jnode_t *jl = json_arr_at(layers, i);
        const jnode_t *t;
        if (strcmp(json_str(jl, "op", ""), "bdense") != 0) {
            strncpy(err, "not an all-bdense model", errlen - 1);
            return 1;
        }
        bl[i].n_in = (int)json_num(jl, "n_in", 0);
        bl[i].n_out = (int)json_num(jl, "n_out", 0);
        bl[i].first_u8 = json_num(jl, "first_layer_u8", 0) != 0.0;
        t = json_get(jl, "w");
        bl[i].w = rim_out->weights + (size_t)json_num(t, "off", 0);
        t = json_get(jl, "thresh");
        bl[i].thresh =
            t ? (const int *)(rim_out->weights +
                              (size_t)json_num(t, "off", 0))
              : NULL;
    }
    *n_bl = n;
    /* root/manifest strings stay alive via rim_out */
    return 0;
}

/* GPU path for one batch: images[B][3072] u8 -> labels[B] */
static int bnn_gpu_batch(const bnn_gpu_t *gb, const blayer_t *bl, int n_bl,
                         const unsigned char *imgs, int B,
                         unsigned char *labels, char *err, size_t errlen)
{
    /* H: byte-per-bit activations [B][width] (0/1) */
    static unsigned char H[GPU_BATCH * 1024], Hn[GPU_BATCH * 1024];
    static unsigned char Wt[256 * 256];      /* B-tile: W^T chunk, 0/1 bytes */
    static int Msum[GPU_BATCH * 1024];
    int li, b, j, k;

    for (li = 0; li < n_bl; li++) {
        const blayer_t *L = &bl[li];
        if (L->first_u8) {
            /* integer first layer on CPU */
            for (b = 0; b < B; b++) {
                const unsigned char *x = imgs + (size_t)b * L->n_in;
                for (j = 0; j < L->n_out; j++) {
                    const unsigned char *wrow =
                        L->w + (size_t)j * (L->n_in / 8);
                    long sum = 0;
                    for (k = 0; k < L->n_in; k++)
                        sum += ((wrow[k >> 3] >> (k & 7)) & 1)
                                   ? (long)x[k]
                                   : -(long)x[k];
                    H[(size_t)b * L->n_out + j] =
                        sum >= (long)L->thresh[j];
                }
            }
            continue;
        }
        /* binary layer on GPU: matches[B][n_out] via 256-tiles */
        memset(Msum, 0, (size_t)B * L->n_out * sizeof(int));
        for (k = 0; k < L->n_in; k += 256) {
            int kk = L->n_in - k < 256 ? L->n_in - k : 256;
            int j0;
            for (j0 = 0; j0 < L->n_out; j0 += 256) {
                int jj = L->n_out - j0 < 256 ? L->n_out - j0 : 256;
                int bi, jx, kx;
                int Mtile[GPU_BATCH * 256];
                /* A tile: H[b][k..k+kk] ; B tile: W^T[k..][j0..] */
                static unsigned char At[GPU_BATCH * 256];
                for (bi = 0; bi < B; bi++)
                    memcpy(At + (size_t)bi * kk,
                           H + (size_t)bi * L->n_in + k, (size_t)kk);
                for (kx = 0; kx < kk; kx++)
                    for (jx = 0; jx < jj; jx++) {
                        const unsigned char *wrow =
                            L->w + (size_t)(j0 + jx) * (L->n_in / 8);
                        Wt[kx * jj + jx] =
                            (wrow[(k + kx) >> 3] >> ((k + kx) & 7)) & 1;
                    }
                memset(Mtile, 0, (size_t)B * jj * sizeof(int));
                if (gb->bgemm(B, jj, kk, At, Wt, Mtile, err, errlen))
                    return 1;
                for (bi = 0; bi < B; bi++)
                    for (jx = 0; jx < jj; jx++)
                        Msum[(size_t)bi * L->n_out + j0 + jx] +=
                            Mtile[(size_t)bi * jj + jx];
            }
        }
        if (L->thresh) {
            for (b = 0; b < B; b++)
                for (j = 0; j < L->n_out; j++) {
                    long s = 2L * Msum[(size_t)b * L->n_out + j] - L->n_in;
                    Hn[(size_t)b * L->n_out + j] =
                        s >= (long)L->thresh[j];
                }
            memcpy(H, Hn, (size_t)B * L->n_out);
        } else {
            for (b = 0; b < B; b++) {
                long best = -0x7FFFFFFFL;
                int bestj = 0;
                for (j = 0; j < L->n_out; j++) {
                    long s = 2L * Msum[(size_t)b * L->n_out + j] - L->n_in;
                    if (s > best) {
                        best = s;
                        bestj = j;
                    }
                }
                labels[b] = (unsigned char)bestj;
            }
        }
    }
    return 0;
}

int bnn_eval(const char *mpath, const char *ipath, const char *lpath, int N,
             const char *backend)
{
    model_t *m;
    rim_model_t rim2;
    blayer_t bl[MAX_BL];
    int n_bl = 0;
    char err[160];
    FILE *f;
    unsigned char *imgs, *labels, *cpu_pred, *gpu_pred;
    int in_bytes, i, correct_cpu = 0, use_gpu;
    double t0, t_cpu, t_gpu = 0.0;

    memset(&rim2, 0, sizeof(rim2));
    if (model_open(mpath, &m, err, sizeof(err))) {
        printf("bnn-eval: FAIL: %s\n", err);
        return 1;
    }
    if (parse_bnn(m, mpath, bl, &n_bl, &rim2, err, sizeof(err))) {
        printf("bnn-eval: FAIL: %s\n", err);
        model_close(m);
        return 1;
    }
    in_bytes = model_input_bytes(m);

    imgs = (unsigned char *)malloc((size_t)N * in_bytes);
    labels = (unsigned char *)malloc((size_t)N);
    cpu_pred = (unsigned char *)malloc((size_t)N);
    gpu_pred = (unsigned char *)malloc((size_t)N);
    f = fopen(ipath, "rb");
    if (!f || !imgs || !labels || !cpu_pred || !gpu_pred ||
        fread(imgs, 1, (size_t)N * in_bytes, f) != (size_t)N * in_bytes) {
        printf("bnn-eval: FAIL: cannot read images\n");
        return 1;
    }
    fclose(f);
    f = fopen(lpath, "rb");
    if (!f || fread(labels, 1, (size_t)N, f) != (size_t)N) {
        printf("bnn-eval: FAIL: cannot read labels\n");
        return 1;
    }
    fclose(f);

    /* CPU reference path */
    t0 = ri_now();
    {
        float logits[256];
        for (i = 0; i < N; i++) {
            int c, best = 0;
            if (model_infer(m, imgs + (size_t)i * in_bytes, logits)) {
                printf("bnn-eval: FAIL: cpu infer\n");
                return 1;
            }
            for (c = 1; c < model_n_classes(m); c++)
                if (logits[c] > logits[best])
                    best = c;
            cpu_pred[i] = (unsigned char)best;
            if (best == labels[i])
                correct_cpu++;
        }
    }
    t_cpu = ri_now() - t0;
    printf("bnn.cpu.top1=%.4f bnn.cpu.img_per_sec=%.2f\n",
           (double)correct_cpu / N, N / t_cpu);

    {
        const bnn_gpu_t *gb = NULL;
        if (backend && strcmp(backend, "glide") == 0)
            gb = &BNN_GLIDE;
        else if (backend && strcmp(backend, "nvgl") == 0)
            gb = &BNN_NVGL;
        use_gpu = gb != NULL;
        if (use_gpu) {
            int agree = 0, correct_gpu = 0, b0;
            if (gb->init(err, sizeof(err))) {
                printf("bnn.%s.init=FAIL %s\n", gb->tag, err);
                printf("bnn-eval: FAIL\n");
                return 1;
            }
            t0 = ri_now();
            for (b0 = 0; b0 < N; b0 += GPU_BATCH) {
                int B = N - b0 < GPU_BATCH ? N - b0 : GPU_BATCH;
                if (bnn_gpu_batch(gb, bl, n_bl, imgs + (size_t)b0 * in_bytes,
                                  B, gpu_pred + b0, err, sizeof(err))) {
                    printf("bnn.%s=FAIL %s\n", gb->tag, err);
                    gb->shutdown();
                    printf("bnn-eval: FAIL\n");
                    return 1;
                }
            }
            t_gpu = ri_now() - t0;
            gb->shutdown();
            for (i = 0; i < N; i++) {
                if (gpu_pred[i] == cpu_pred[i])
                    agree++;
                if (gpu_pred[i] == labels[i])
                    correct_gpu++;
            }
            printf("bnn.%s.top1=%.4f bnn.%s.img_per_sec=%.2f\n", gb->tag,
                   (double)correct_gpu / N, gb->tag, N / t_gpu);
            printf("bnn.label_agreement=%d/%d %s\n", agree, N,
                   agree == N ? "EXACT" : "MISMATCH");
            printf("bnn-eval: %s\n", agree == N ? "OK" : "MISMATCH");
            model_close(m);
            rim_free(&rim2);
            return agree != N;
        }
    }
    printf("bnn-eval: OK\n");
    model_close(m);
    rim_free(&rim2);
    return 0;
}
