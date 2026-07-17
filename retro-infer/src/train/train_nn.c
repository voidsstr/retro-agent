/*
 * On-device MLP / logistic-regression trainer (M2).
 *
 * Minibatch SGD + momentum, softmax cross-entropy, dense layers with ReLU
 * between. Arch "784,128,10" = 1 hidden layer; "784,10" = logistic
 * regression. Deterministic: fixed xorshift PRNG for init + shuffle, fixed
 * accumulation order in the kernels.
 *
 * Data files: images u8 (features, /255 on load) + labels u8 — same formats
 * as --eval. Trained model saved as a dense-f32 .rim (loadable by --eval).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../infer.h"
#include "../port.h"
#include "tutil.h"
#include "train_nn.h"

#define MAX_NN_LAYERS 4

typedef struct {
    int dims[MAX_NN_LAYERS + 1];
    int n_layers;                       /* dense layer count */
    float *W[MAX_NN_LAYERS], *b[MAX_NN_LAYERS];
    float *vW[MAX_NN_LAYERS], *vb[MAX_NN_LAYERS];
} nn_t;

static void nn_free(nn_t *nn)
{
    int l;
    for (l = 0; l < nn->n_layers; l++) {
        free(nn->W[l]); free(nn->b[l]);
        free(nn->vW[l]); free(nn->vb[l]);
    }
}

static int nn_alloc(nn_t *nn, rng_t *rng)
{
    int l;
    for (l = 0; l < nn->n_layers; l++) {
        int in = nn->dims[l], out = nn->dims[l + 1];
        size_t wn = (size_t)in * out;
        float sd = (float)sqrt(2.0 / (double)in);
        size_t i;
        nn->W[l] = (float *)malloc(wn * sizeof(float));
        nn->b[l] = (float *)calloc((size_t)out, sizeof(float));
        nn->vW[l] = (float *)calloc(wn, sizeof(float));
        nn->vb[l] = (float *)calloc((size_t)out, sizeof(float));
        if (!nn->W[l] || !nn->b[l] || !nn->vW[l] || !nn->vb[l])
            return 1;
        for (i = 0; i < wn; i++)
            nn->W[l][i] = rng_normal(rng) * sd;
    }
    return 0;
}

/* forward for a batch: X [B,in] -> per-layer Z (pre-act) and A (post-act);
 * final layer output left as logits in Z[last]. A[0] = X. */
typedef struct {
    float *Z[MAX_NN_LAYERS];   /* [B, dims[l+1]] */
    float *A[MAX_NN_LAYERS + 1];
} nn_buf_t;

static void nn_forward(const nn_t *nn, nn_buf_t *bf, int B)
{
    int l;
    for (l = 0; l < nn->n_layers; l++) {
        int in = nn->dims[l], out = nn->dims[l + 1];
        int i, j;
        /* Z = A W^T + b : gemm_nt(M=B, N=out, K=in) */
        g_kernels.gemm_f32_nt(B, out, in, bf->A[l], nn->W[l], bf->Z[l], 1);
        for (i = 0; i < B; i++)
            for (j = 0; j < out; j++)
                bf->Z[l][(size_t)i * out + j] += nn->b[l][j];
        if (l < nn->n_layers - 1) {
            size_t n = (size_t)B * out;
            size_t k;
            for (k = 0; k < n; k++)
                bf->A[l + 1][k] = bf->Z[l][k] > 0.0f ? bf->Z[l][k] : 0.0f;
        }
    }
}

/* softmax rows in place; returns mean CE loss vs labels */
static double softmax_ce(float *Z, const unsigned char *lab, int B, int C)
{
    double loss = 0.0;
    int i, j;
    for (i = 0; i < B; i++) {
        float *z = Z + (size_t)i * C;
        float mx = z[0];
        double sum = 0.0;
        for (j = 1; j < C; j++)
            if (z[j] > mx)
                mx = z[j];
        for (j = 0; j < C; j++) {
            double e = exp((double)(z[j] - mx));
            z[j] = (float)e;
            sum += e;
        }
        for (j = 0; j < C; j++)
            z[j] = (float)((double)z[j] / sum);
        {
            double p = (double)z[lab[i]];
            if (p < 1e-12)
                p = 1e-12;
            loss -= log(p);
        }
    }
    return loss / (double)B;
}

int train_nn_run(const train_nn_cfg_t *cfg)
{
    nn_t nn;
    rng_t rng;
    unsigned char *Xtr = NULL, *ytr = NULL, *Xte = NULL, *yte = NULL;
    int *order = NULL;
    nn_buf_t bf;
    float *dZ[MAX_NN_LAYERS];       /* grad wrt pre-activations */
    float *dW = NULL, *db = NULL;   /* scratch, sized for largest layer */
    int F, C, B = cfg->batch, l, e, i, j;
    size_t max_w = 0;
    int max_dim = 0;
    double t_start;
    FILE *f;

    memset(&nn, 0, sizeof(nn));
    memset(&bf, 0, sizeof(bf));
    memset(dZ, 0, sizeof(dZ));

    /* parse arch */
    {
        const char *p = cfg->arch;
        nn.n_layers = 0;
        i = 0;
        while (*p && i <= MAX_NN_LAYERS) {
            nn.dims[i++] = atoi(p);
            while (*p && *p != ',')
                p++;
            if (*p == ',')
                p++;
        }
        nn.n_layers = i - 1;
        if (nn.n_layers < 1) {
            printf("train: FAIL: bad arch\n");
            return 1;
        }
    }
    F = nn.dims[0];
    C = nn.dims[nn.n_layers];

    /* load data */
    {
        size_t need_tr = (size_t)cfg->n_train * F, need_te = (size_t)cfg->n_test * F;
        f = fopen(cfg->train_x, "rb");
        Xtr = (unsigned char *)malloc(need_tr);
        if (!f || !Xtr || fread(Xtr, 1, need_tr, f) != need_tr) {
            printf("train: FAIL: cannot read %s\n", cfg->train_x);
            if (f) fclose(f);
            goto fail;
        }
        fclose(f);
        f = fopen(cfg->train_y, "rb");
        ytr = (unsigned char *)malloc((size_t)cfg->n_train);
        if (!f || !ytr || fread(ytr, 1, (size_t)cfg->n_train, f) != (size_t)cfg->n_train) {
            printf("train: FAIL: cannot read %s\n", cfg->train_y);
            if (f) fclose(f);
            goto fail;
        }
        fclose(f);
        f = fopen(cfg->test_x, "rb");
        Xte = (unsigned char *)malloc(need_te);
        if (!f || !Xte || fread(Xte, 1, need_te, f) != need_te) {
            printf("train: FAIL: cannot read %s\n", cfg->test_x);
            if (f) fclose(f);
            goto fail;
        }
        fclose(f);
        f = fopen(cfg->test_y, "rb");
        yte = (unsigned char *)malloc((size_t)cfg->n_test);
        if (!f || !yte || fread(yte, 1, (size_t)cfg->n_test, f) != (size_t)cfg->n_test) {
            printf("train: FAIL: cannot read %s\n", cfg->test_y);
            if (f) fclose(f);
            goto fail;
        }
        fclose(f);
    }

    rng_seed(&rng, (unsigned)cfg->seed);
    if (nn_alloc(&nn, &rng)) {
        printf("train: FAIL: out of memory (weights)\n");
        goto fail;
    }

    /* batch buffers */
    for (l = 0; l <= nn.n_layers; l++)
        if (nn.dims[l] > max_dim)
            max_dim = nn.dims[l];
    bf.A[0] = (float *)malloc((size_t)B * F * sizeof(float));
    if (!bf.A[0]) goto oom;
    for (l = 0; l < nn.n_layers; l++) {
        int out = nn.dims[l + 1];
        size_t wn = (size_t)nn.dims[l] * out;
        bf.Z[l] = (float *)malloc((size_t)B * out * sizeof(float));
        dZ[l] = (float *)malloc((size_t)B * out * sizeof(float));
        if (!bf.Z[l] || !dZ[l]) goto oom;
        if (l < nn.n_layers - 1) {
            bf.A[l + 1] = (float *)malloc((size_t)B * out * sizeof(float));
            if (!bf.A[l + 1]) goto oom;
        }
        if (wn > max_w)
            max_w = wn;
    }
    dW = (float *)malloc(max_w * sizeof(float));
    db = (float *)malloc((size_t)max_dim * sizeof(float));
    order = (int *)malloc((size_t)cfg->n_train * sizeof(int));
    if (!dW || !db || !order) goto oom;
    for (i = 0; i < cfg->n_train; i++)
        order[i] = i;

    printf("train.arch=%s train.n=%d test.n=%d batch=%d lr=%g mom=%g seed=%d\n",
           cfg->arch, cfg->n_train, cfg->n_test, B, (double)cfg->lr,
           (double)cfg->momentum, cfg->seed);
    printf("train.kernel_f32=%s\n", g_kernels.gemm_f32_name);
    t_start = ri_now();

    for (e = 0; e < cfg->epochs; e++) {
        double ep_loss = 0.0;
        int n_batches = 0, bstart;
        rng_shuffle(&rng, order, cfg->n_train);
        for (bstart = 0; bstart + B <= cfg->n_train; bstart += B) {
            /* gather batch */
            for (i = 0; i < B; i++) {
                const unsigned char *src = Xtr + (size_t)order[bstart + i] * F;
                float *dst = bf.A[0] + (size_t)i * F;
                for (j = 0; j < F; j++)
                    dst[j] = (float)src[j] / 255.0f;
            }
            nn_forward(&nn, &bf, B);
            {
                unsigned char blab[1024];
                for (i = 0; i < B; i++)
                    blab[i] = ytr[order[bstart + i]];
                ep_loss += softmax_ce(bf.Z[nn.n_layers - 1], blab, B, C);
                n_batches++;
                /* dZ_last = (P - onehot)/B */
                for (i = 0; i < B; i++) {
                    float *p = bf.Z[nn.n_layers - 1] + (size_t)i * C;
                    float *d = dZ[nn.n_layers - 1] + (size_t)i * C;
                    for (j = 0; j < C; j++)
                        d[j] = p[j] / (float)B;
                    d[blab[i]] -= 1.0f / (float)B;
                }
            }
            /* backward */
            for (l = nn.n_layers - 1; l >= 0; l--) {
                int in = nn.dims[l], out = nn.dims[l + 1];
                size_t wn = (size_t)in * out;
                size_t k;
                /* dW[out,in] = dZ^T A : gemm_tn(M=out,N=in,K=B) */
                g_kernels.gemm_f32_tn(out, in, B, dZ[l], bf.A[l], dW, 1);
                for (j = 0; j < out; j++)
                    db[j] = 0.0f;
                for (i = 0; i < B; i++)
                    for (j = 0; j < out; j++)
                        db[j] += dZ[l][(size_t)i * out + j];
                if (l > 0) {
                    /* dA_prev = dZ W : gemm(M=B,N=in,K=out) then relu mask */
                    g_kernels.gemm_f32(B, in, out, dZ[l], nn.W[l],
                                       dZ[l - 1], 1);
                    for (k = 0; k < (size_t)B * in; k++)
                        if (bf.Z[l - 1][k] <= 0.0f)
                            dZ[l - 1][k] = 0.0f;
                }
                /* momentum update */
                for (k = 0; k < wn; k++) {
                    nn.vW[l][k] = cfg->momentum * nn.vW[l][k] -
                                  cfg->lr * dW[k];
                    nn.W[l][k] += nn.vW[l][k];
                }
                for (j = 0; j < out; j++) {
                    nn.vb[l][j] = cfg->momentum * nn.vb[l][j] -
                                  cfg->lr * db[j];
                    nn.b[l][j] += nn.vb[l][j];
                }
            }
        }

        /* test accuracy (batched forward over the test set) */
        {
            int correct = 0, bs;
            for (bs = 0; bs + B <= cfg->n_test || (bs < cfg->n_test && bs + B > cfg->n_test); bs += B) {
                int bn = cfg->n_test - bs < B ? cfg->n_test - bs : B;
                if (bn < B) {
                    /* pad by repeating last sample; only count bn */
                    for (i = bn; i < B; i++)
                        memset(bf.A[0] + (size_t)i * F, 0, (size_t)F * sizeof(float));
                }
                for (i = 0; i < bn; i++) {
                    const unsigned char *src = Xte + (size_t)(bs + i) * F;
                    float *dst = bf.A[0] + (size_t)i * F;
                    for (j = 0; j < F; j++)
                        dst[j] = (float)src[j] / 255.0f;
                }
                nn_forward(&nn, &bf, B);
                for (i = 0; i < bn; i++) {
                    const float *z = bf.Z[nn.n_layers - 1] + (size_t)i * C;
                    int best = 0;
                    for (j = 1; j < C; j++)
                        if (z[j] > z[best])
                            best = j;
                    if (best == yte[bs + i])
                        correct++;
                }
                if (bn < B)
                    break;
            }
            printf("epoch=%d train_loss=%.6f test_acc=%.4f elapsed=%.1fs\n",
                   e + 1, ep_loss / (double)(n_batches > 0 ? n_batches : 1),
                   (double)correct / (double)cfg->n_test, ri_now() - t_start);
            fflush(stdout);
        }
    }

    if (cfg->out_rim && strcmp(cfg->out_rim, "-") != 0) {
        if (rim_save_dense(cfg->out_rim, nn.dims, nn.n_layers,
                           (const float *const *)nn.W,
                           (const float *const *)nn.b, cfg->input_u8_div255))
            printf("train: WARN: could not save %s\n", cfg->out_rim);
        else
            printf("train.saved=%s\n", cfg->out_rim);
    }
    printf("train: OK\n");

    nn_free(&nn);
    for (l = 0; l < nn.n_layers; l++) {
        free(bf.Z[l]);
        free(dZ[l]);
    }
    for (l = 0; l <= nn.n_layers; l++)
        free(bf.A[l]);
    free(dW); free(db); free(order);
    free(Xtr); free(ytr); free(Xte); free(yte);
    return 0;
oom:
    printf("train: FAIL: out of memory (buffers)\n");
fail:
    nn_free(&nn);
    for (l = 0; l < MAX_NN_LAYERS; l++) {
        free(bf.Z[l]);
        free(dZ[l]);
    }
    for (l = 0; l <= MAX_NN_LAYERS; l++)
        free(bf.A[l]);
    free(dW); free(db); free(order);
    free(Xtr); free(ytr); free(Xte); free(yte);
    return 1;
}
