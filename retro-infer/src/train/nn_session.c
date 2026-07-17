/*
 * Step-wise NN training session for fleet data-parallel SGD (M7).
 *
 * The brain drives one session per node over the serve protocol:
 *   NTINIT arch seed lr mom      create session (identical on every node)
 *   NTSTEP  [X u8 | y u8]        forward+backward a batch shard ->
 *                                 reply [f32 loss][f32 flat grads]
 *   NTAPPLY [f32 flat grads]     momentum update with the allreduced grads
 *   NTEVAL  [X u8 | y u8]        reply "acc=<f> loss=<f>" on the chunk
 *   NTEXPORT <path>              save weights as dense-f32 .rim
 *   NTFREE                       tear down
 *
 * All nodes NTINIT with the same seed so initial weights are identical;
 * NTAPPLY applies the SAME averaged gradient everywhere, keeping weights in
 * lockstep (up to per-node fp rounding, which the M7 acceptance tolerates).
 * Grad layout: W0,b0,W1,b1,... row-major f32 LE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../infer.h"
#include "tutil.h"
#include "train_nn.h"
#include "nn_session.h"

#define MAX_L 4

struct nn_session {
    int dims[MAX_L + 1];
    int n_layers;
    float lr, momentum;
    float *W[MAX_L], *b[MAX_L], *vW[MAX_L], *vb[MAX_L];
    size_t n_params;
};

static size_t layer_wn(const nn_session_t *s, int l)
{
    return (size_t)s->dims[l] * s->dims[l + 1];
}

nn_session_t *nns_create(const char *arch, unsigned seed, float lr,
                         float momentum)
{
    nn_session_t *s = (nn_session_t *)calloc(1, sizeof(nn_session_t));
    rng_t rng;
    const char *p = arch;
    int i = 0, l;

    if (!s)
        return NULL;
    while (*p && i <= MAX_L) {
        s->dims[i++] = atoi(p);
        while (*p && *p != ',')
            p++;
        if (*p == ',')
            p++;
    }
    s->n_layers = i - 1;
    if (s->n_layers < 1) {
        free(s);
        return NULL;
    }
    s->lr = lr;
    s->momentum = momentum;
    rng_seed(&rng, seed);
    for (l = 0; l < s->n_layers; l++) {
        size_t wn = layer_wn(s, l);
        float sd = (float)sqrt(2.0 / (double)s->dims[l]);
        size_t k;
        s->W[l] = (float *)malloc(wn * sizeof(float));
        s->b[l] = (float *)calloc((size_t)s->dims[l + 1], sizeof(float));
        s->vW[l] = (float *)calloc(wn, sizeof(float));
        s->vb[l] = (float *)calloc((size_t)s->dims[l + 1], sizeof(float));
        if (!s->W[l] || !s->b[l] || !s->vW[l] || !s->vb[l]) {
            nns_free(s);
            return NULL;
        }
        for (k = 0; k < wn; k++)
            s->W[l][k] = rng_normal(&rng) * sd;
        s->n_params += wn + (size_t)s->dims[l + 1];
    }
    return s;
}

void nns_free(nn_session_t *s)
{
    int l;
    if (!s)
        return;
    for (l = 0; l < s->n_layers; l++) {
        free(s->W[l]); free(s->b[l]);
        free(s->vW[l]); free(s->vb[l]);
    }
    free(s);
}

size_t nns_n_params(const nn_session_t *s) { return s->n_params; }
int nns_input_dim(const nn_session_t *s) { return s->dims[0]; }

/* forward+backward one batch; grads_out has n_params floats. Returns loss. */
double nns_step(nn_session_t *s, const unsigned char *X,
                const unsigned char *y, int B, float *grads_out)
{
    int in0 = s->dims[0], C = s->dims[s->n_layers];
    float *A[MAX_L + 1] = {0}, *Z[MAX_L] = {0}, *dZ[MAX_L] = {0};
    double loss = 0.0;
    int l, i, j;
    float *gp;

    A[0] = (float *)malloc((size_t)B * in0 * sizeof(float));
    if (!A[0])
        return -1.0;
    for (i = 0; i < B; i++)
        for (j = 0; j < in0; j++)
            A[0][(size_t)i * in0 + j] = (float)X[(size_t)i * in0 + j] / 255.0f;

    for (l = 0; l < s->n_layers; l++) {
        int out = s->dims[l + 1];
        Z[l] = (float *)malloc((size_t)B * out * sizeof(float));
        dZ[l] = (float *)malloc((size_t)B * out * sizeof(float));
        if (l < s->n_layers - 1)
            A[l + 1] = (float *)malloc((size_t)B * out * sizeof(float));
        if (!Z[l] || !dZ[l] || (l < s->n_layers - 1 && !A[l + 1]))
            goto oom;
    }

    /* forward */
    for (l = 0; l < s->n_layers; l++) {
        int out = s->dims[l + 1];
        size_t n, k;
        g_kernels.gemm_f32_nt(B, out, s->dims[l], A[l], s->W[l], Z[l], 1);
        for (i = 0; i < B; i++)
            for (j = 0; j < out; j++)
                Z[l][(size_t)i * out + j] += s->b[l][j];
        if (l < s->n_layers - 1) {
            n = (size_t)B * out;
            for (k = 0; k < n; k++)
                A[l + 1][k] = Z[l][k] > 0.0f ? Z[l][k] : 0.0f;
        }
    }

    /* softmax CE + dZ_last */
    for (i = 0; i < B; i++) {
        float *z = Z[s->n_layers - 1] + (size_t)i * C;
        float mx = z[0];
        double sum = 0.0, pl;
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
        pl = (double)z[y[i]];
        if (pl < 1e-12)
            pl = 1e-12;
        loss -= log(pl);
        {
            float *d = dZ[s->n_layers - 1] + (size_t)i * C;
            for (j = 0; j < C; j++)
                d[j] = z[j] / (float)B;
            d[y[i]] -= 1.0f / (float)B;
        }
    }
    loss /= (double)B;

    /* backward, writing grads into grads_out in W0,b0,W1,b1.. order */
    gp = grads_out;
    /* need per-layer positions; compute offsets first */
    {
        size_t off = 0;
        float *gW[MAX_L], *gb[MAX_L];
        for (l = 0; l < s->n_layers; l++) {
            gW[l] = grads_out + off;
            off += layer_wn(s, l);
            gb[l] = grads_out + off;
            off += (size_t)s->dims[l + 1];
        }
        (void)gp;
        for (l = s->n_layers - 1; l >= 0; l--) {
            int out = s->dims[l + 1], in = s->dims[l];
            size_t k;
            g_kernels.gemm_f32_tn(out, in, B, dZ[l], A[l], gW[l], 1);
            for (j = 0; j < out; j++)
                gb[l][j] = 0.0f;
            for (i = 0; i < B; i++)
                for (j = 0; j < out; j++)
                    gb[l][j] += dZ[l][(size_t)i * out + j];
            if (l > 0) {
                g_kernels.gemm_f32(B, in, out, dZ[l], s->W[l], dZ[l - 1], 1);
                for (k = 0; k < (size_t)B * in; k++)
                    if (Z[l - 1][k] <= 0.0f)
                        dZ[l - 1][k] = 0.0f;
            }
        }
    }

    for (l = 0; l < s->n_layers; l++) {
        free(A[l]); free(Z[l]); free(dZ[l]);
    }
    free(A[s->n_layers]);
    return loss;
oom:
    for (l = 0; l <= s->n_layers; l++)
        free(A[l]);
    for (l = 0; l < s->n_layers; l++) {
        free(Z[l]); free(dZ[l]);
    }
    return -1.0;
}

void nns_apply(nn_session_t *s, const float *grads)
{
    size_t off = 0;
    int l, j;
    for (l = 0; l < s->n_layers; l++) {
        size_t wn = layer_wn(s, l), k;
        for (k = 0; k < wn; k++) {
            s->vW[l][k] = s->momentum * s->vW[l][k] - s->lr * grads[off + k];
            s->W[l][k] += s->vW[l][k];
        }
        off += wn;
        for (j = 0; j < s->dims[l + 1]; j++) {
            s->vb[l][j] = s->momentum * s->vb[l][j] - s->lr * grads[off + j];
            s->b[l][j] += s->vb[l][j];
        }
        off += (size_t)s->dims[l + 1];
    }
}

/* eval on a chunk: returns correct count, sets *loss_out */
int nns_eval(nn_session_t *s, const unsigned char *X, const unsigned char *y,
             int n, double *loss_out)
{
    int in0 = s->dims[0], C = s->dims[s->n_layers];
    float *a = (float *)malloc((size_t)in0 * sizeof(float));
    float *z0 = (float *)malloc((size_t)1024 * sizeof(float));
    float *z1 = (float *)malloc((size_t)1024 * sizeof(float));
    int i, j, l, correct = 0;
    double loss = 0.0;

    if (!a || !z0 || !z1) {
        free(a); free(z0); free(z1);
        return -1;
    }
    for (i = 0; i < n; i++) {
        float *cur = a, *nxt;
        int cur_n = in0;
        for (j = 0; j < in0; j++)
            a[j] = (float)X[(size_t)i * in0 + j] / 255.0f;
        for (l = 0; l < s->n_layers; l++) {
            int out = s->dims[l + 1];
            nxt = (cur == z0) ? z1 : z0;
            g_kernels.gemm_f32_nt(1, out, cur_n, cur, s->W[l], nxt, 1);
            for (j = 0; j < out; j++)
                nxt[j] += s->b[l][j];
            if (l < s->n_layers - 1)
                for (j = 0; j < out; j++)
                    if (nxt[j] < 0.0f)
                        nxt[j] = 0.0f;
            cur = nxt;
            cur_n = out;
        }
        {
            int best = 0;
            float mx = cur[0];
            double sum = 0.0, pl;
            for (j = 1; j < C; j++)
                if (cur[j] > mx) {
                    mx = cur[j];
                    best = j;
                }
            if (best == y[i])
                correct++;
            for (j = 0; j < C; j++)
                sum += exp((double)(cur[j] - mx));
            pl = 1.0 / sum;   /* prob of argmax... need prob of true label */
            pl = exp((double)(cur[y[i]] - mx)) / sum;
            if (pl < 1e-12)
                pl = 1e-12;
            loss -= log(pl);
        }
    }
    free(a); free(z0); free(z1);
    *loss_out = loss / (double)n;
    return correct;
}

int nns_export(nn_session_t *s, const char *path)
{
    return rim_save_dense(path, s->dims, s->n_layers,
                          (const float *const *)s->W,
                          (const float *const *)s->b, 1);
}
