/*
 * .rim model executor (batch-1 inference). Parses the manifest per
 * tools/rim/FORMAT.md, builds a layer list pointing into the weights blob,
 * and runs the ping-pong forward pass. Int8 math exactly as specced:
 * i32 accumulate, fp32 requant multiplier, half-away-from-zero rounding.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "infer.h"
#include "json.h"
#include "exec.h"
#include "ops/nn.h"

#define MAX_LAYERS 64

typedef enum {
    OP_CONV2D, OP_DENSE, OP_RELU, OP_MAXPOOL, OP_FLATTEN, OP_SOFTMAX, OP_KNN
} lop_t;

typedef struct {
    lop_t op;
    int is_i8;
    int in_ch, out_ch, k, stride, pad;         /* conv2d / maxpool */
    int in_n, out_n;                           /* dense */
    const void *w;                             /* conv/dense weights */
    const void *b;                             /* bias (f32* or i32*) */
    float w_scale, in_scale, out_scale;
    int knn_k, n_train, n_feat;
    const unsigned char *train;
    const unsigned char *train_labels;
} layer_t;

struct model {
    rim_model_t rim;
    jnode_t *root;
    layer_t layers[MAX_LAYERS];
    int n_layers;
    int in_c, in_h, in_w;
    float in_div;
    int in_is_u8;
    int n_classes;
    int out_elems;       /* final activation size from shape trace */
    int skip_softmax;    /* 1: stop before a trailing softmax (logit dumps) */
};

static void set_err(char *errbuf, size_t errlen, const char *msg)
{
    if (errbuf && errlen) {
        strncpy(errbuf, msg, errlen - 1);
        errbuf[errlen - 1] = '\0';
    }
}

/* Resolve a tensor-ref object {"off","dtype","shape","scale"} to a blob ptr */
static const void *tref(const model_t *m, const jnode_t *layer,
                        const char *key, float *scale_out)
{
    const jnode_t *t = json_get(layer, key);
    size_t off;
    if (!t || t->type != J_OBJ)
        return NULL;
    off = (size_t)json_num(t, "off", 0);
    if (scale_out)
        *scale_out = (float)json_num(t, "scale", 0.0);
    if (off >= m->rim.weights_len)
        return NULL;
    return m->rim.weights + off;
}

int model_open(const char *path, model_t **out, char *errbuf, size_t errlen)
{
    model_t *m;
    const jnode_t *input, *layers, *labels;
    char rerr[128];
    int i, n;

    m = (model_t *)calloc(1, sizeof(model_t));
    if (!m) {
        set_err(errbuf, errlen, "out of memory");
        return 1;
    }
    if (rim_load(path, &m->rim, rerr, sizeof(rerr)) != 0) {
        set_err(errbuf, errlen, rerr);
        free(m);
        return 1;
    }
    m->root = json_parse(m->rim.manifest_json);
    if (!m->root) {
        set_err(errbuf, errlen, "manifest JSON parse error");
        rim_free(&m->rim);
        free(m);
        return 1;
    }

    input = json_get(m->root, "input");
    if (input) {
        const jnode_t *shape = json_get(input, "shape");
        int nd = json_arr_len(shape);
        m->in_c = nd >= 1 ? (int)json_arr_at(shape, 0)->num : 1;
        m->in_h = nd >= 2 ? (int)json_arr_at(shape, 1)->num : 1;
        m->in_w = nd >= 3 ? (int)json_arr_at(shape, 2)->num : 1;
        if (nd == 1) {                     /* [N_features] tabular */
            m->in_h = m->in_c;
            m->in_c = 1;
            m->in_w = 1;
        }
        m->in_div = (float)json_num(input, "div", 0.0);
        m->in_is_u8 = strcmp(json_str(input, "dtype", "f32"), "u8") == 0;
    }

    layers = json_get(m->root, "layers");
    n = json_arr_len(layers);
    if (n <= 0 || n > MAX_LAYERS) {
        set_err(errbuf, errlen, "bad layer count");
        model_close(m);
        return 1;
    }
    for (i = 0; i < n; i++) {
        const jnode_t *jl = json_arr_at(layers, i);
        layer_t *L = &m->layers[i];
        const char *op = json_str(jl, "op", "");
        L->is_i8 = strcmp(json_str(jl, "dtype", "f32"), "i8") == 0;
        if (strcmp(op, "conv2d") == 0) {
            L->op = OP_CONV2D;
            L->in_ch = (int)json_num(jl, "in_ch", 0);
            L->out_ch = (int)json_num(jl, "out_ch", 0);
            L->k = (int)json_num(jl, "k", 0);
            L->stride = (int)json_num(jl, "stride", 1);
            L->pad = (int)json_num(jl, "pad", 0);
            L->w = tref(m, jl, "w", &L->w_scale);
            L->b = tref(m, jl, "b", NULL);
            L->in_scale = (float)json_num(jl, "act_scale_in", 0.0);
            L->out_scale = (float)json_num(jl, "act_scale_out", 0.0);
            if (!L->w) {
                set_err(errbuf, errlen, "conv2d missing weights");
                model_close(m);
                return 1;
            }
        } else if (strcmp(op, "dense") == 0) {
            L->op = OP_DENSE;
            L->in_n = (int)json_num(jl, "in", 0);
            L->out_n = (int)json_num(jl, "out", 0);
            L->w = tref(m, jl, "w", &L->w_scale);
            L->b = tref(m, jl, "b", NULL);
            L->in_scale = (float)json_num(jl, "act_scale_in", 0.0);
            L->out_scale = (float)json_num(jl, "act_scale_out", 0.0);
            m->n_classes = L->out_n;
            if (!L->w) {
                set_err(errbuf, errlen, "dense missing weights");
                model_close(m);
                return 1;
            }
        } else if (strcmp(op, "relu") == 0) {
            L->op = OP_RELU;
        } else if (strcmp(op, "maxpool") == 0) {
            L->op = OP_MAXPOOL;
            L->k = (int)json_num(jl, "k", 2);
            L->stride = (int)json_num(jl, "stride", L->k);
        } else if (strcmp(op, "flatten") == 0) {
            L->op = OP_FLATTEN;
        } else if (strcmp(op, "softmax") == 0) {
            L->op = OP_SOFTMAX;
        } else if (strcmp(op, "knn") == 0) {
            L->op = OP_KNN;
            L->knn_k = (int)json_num(jl, "k", 3);
            L->n_train = (int)json_num(jl, "n_train", 0);
            L->n_feat = (int)json_num(jl, "n_feat", 0);
            L->train = (const unsigned char *)tref(m, jl, "train", NULL);
            L->train_labels =
                (const unsigned char *)tref(m, jl, "train_labels", NULL);
            if (!L->train || !L->train_labels) {
                set_err(errbuf, errlen, "knn missing train tensors");
                model_close(m);
                return 1;
            }
        } else {
            set_err(errbuf, errlen, "unknown layer op");
            model_close(m);
            return 1;
        }
    }
    m->n_layers = n;

    labels = json_get(m->root, "labels");
    if (labels && json_arr_len(labels) > 0)
        m->n_classes = json_arr_len(labels);
    if (m->n_classes == 0 && m->layers[0].op == OP_KNN) {
        const layer_t *L = &m->layers[0];
        int mx = 0, j;
        for (j = 0; j < L->n_train; j++)
            if (L->train_labels[j] > mx)
                mx = L->train_labels[j];
        m->n_classes = mx + 1;
    }

    /* shape trace: validates dims and yields the final activation size */
    {
        int C = m->in_c, H = m->in_h, W = m->in_w;
        for (i = 0; i < m->n_layers; i++) {
            const layer_t *L = &m->layers[i];
            if (L->op == OP_CONV2D) {
                H = (H + 2 * L->pad - L->k) / L->stride + 1;
                W = (W + 2 * L->pad - L->k) / L->stride + 1;
                C = L->out_ch;
            } else if (L->op == OP_MAXPOOL) {
                H = (H - L->k) / L->stride + 1;
                W = (W - L->k) / L->stride + 1;
            } else if (L->op == OP_DENSE) {
                if (L->in_n != C * H * W) {
                    set_err(errbuf, errlen, "dense in-dim mismatch");
                    model_close(m);
                    return 1;
                }
                C = L->out_n; H = 1; W = 1;
            } else if (L->op == OP_FLATTEN) {
                C = C * H * W; H = 1; W = 1;
            } else if (L->op == OP_KNN) {
                C = m->n_classes; H = 1; W = 1;
            }
            if (C <= 0 || H <= 0 || W <= 0) {
                set_err(errbuf, errlen, "bad layer geometry");
                model_close(m);
                return 1;
            }
        }
        m->out_elems = C * H * W;
    }
    if (m->n_classes <= 0)
        m->n_classes = m->out_elems;

    *out = m;
    return 0;
}

void model_set_skip_softmax(model_t *m, int skip)
{
    m->skip_softmax = skip;
}

int model_n_classes(const model_t *m) { return m->n_classes; }
int model_input_bytes(const model_t *m)
{
    int n = m->in_c * m->in_h * m->in_w;
    return m->in_is_u8 ? n : n * 4;
}
const char *model_name(const model_t *m)
{
    return json_str(m->root, "name", "(unnamed)");
}

static int knn_predict(const layer_t *L, const unsigned char *x,
                       float *logits, int n_classes)
{
    /* top-k by L2 (i32); iterate train in order, replace current worst only
     * on strictly smaller distance (earlier index wins ties) */
    long best_d[32];
    int best_i[32], kk = L->knn_k, t, j, c;
    int votes[256];
    if (kk > 32)
        kk = 32;
    for (j = 0; j < kk; j++) {
        best_d[j] = 0x7FFFFFFFL;
        best_i[j] = -1;
    }
    for (t = 0; t < L->n_train; t++) {
        const unsigned char *tr = L->train + (size_t)t * L->n_feat;
        long d = 0;
        int worst = 0;
        for (j = 0; j < L->n_feat; j++) {
            long diff = (long)x[j] - (long)tr[j];
            d += diff * diff;
        }
        for (j = 1; j < kk; j++)
            if (best_d[j] > best_d[worst])
                worst = j;
        if (d < best_d[worst]) {
            best_d[worst] = d;
            best_i[worst] = t;
        }
    }
    memset(votes, 0, sizeof(votes));
    for (j = 0; j < kk; j++)
        if (best_i[j] >= 0)
            votes[L->train_labels[best_i[j]]]++;
    for (c = 0; c < n_classes; c++)
        logits[c] = (float)votes[c];
    return 0;
}

/* Forward pass. input: raw bytes per model_input_bytes(). logits: n_classes */
int model_infer(model_t *m, const void *input, float *logits)
{
    /* activation state */
    float *af = NULL;
    signed char *ai = NULL;
    float a_scale = 0.0f;
    int C = m->in_c, H = m->in_h, W = m->in_w;
    int li, n_elem = C * H * W;
    int ret = 1;

    if (m->layers[0].op == OP_KNN)
        return knn_predict(&m->layers[0], (const unsigned char *)input,
                           logits, m->n_classes);

    /* input -> f32 */
    af = (float *)malloc((size_t)n_elem * sizeof(float));
    if (!af)
        return 1;
    if (m->in_is_u8) {
        const unsigned char *u = (const unsigned char *)input;
        float div = m->in_div > 0.0f ? m->in_div : 1.0f;
        int i;
        for (i = 0; i < n_elem; i++)
            af[i] = (float)u[i] / div;
    } else {
        memcpy(af, input, (size_t)n_elem * sizeof(float));
        if (m->in_div > 0.0f) {
            int i;
            for (i = 0; i < n_elem; i++)
                af[i] /= m->in_div;
        }
    }

    for (li = 0; li < m->n_layers; li++) {
        layer_t *L = &m->layers[li];
        if (m->skip_softmax && li == m->n_layers - 1 && L->op == OP_SOFTMAX)
            break;
        switch (L->op) {
        case OP_CONV2D: {
            int oH = (H + 2 * L->pad - L->k) / L->stride + 1;
            int oW = (W + 2 * L->pad - L->k) / L->stride + 1;
            int K = L->in_ch * L->k * L->k;
            int N = oH * oW;
            if (L->is_i8) {
                signed char *col, *no_i = NULL;
                int *acc;
                float *no_f = NULL;
                /* quantize input if still f32 */
                if (!ai) {
                    int i;
                    ai = (signed char *)malloc((size_t)n_elem);
                    if (!ai) goto done;
                    for (i = 0; i < n_elem; i++)
                        ai[i] = ri_quant_clamp(af[i] / L->in_scale);
                    free(af); af = NULL;
                    a_scale = L->in_scale;
                }
                col = (signed char *)malloc((size_t)K * N);
                acc = (int *)malloc((size_t)L->out_ch * N * sizeof(int));
                if (!col || !acc) { free(col); free(acc); goto done; }
                im2col_i8(ai, C, H, W, L->k, L->stride, L->pad, col, oH, oW);
                g_kernels.gemm_i8(L->out_ch, N, K, (const signed char *)L->w,
                                  col, acc, 1);
                free(col);
                if (L->out_scale > 0.0f) {
                    no_i = (signed char *)malloc((size_t)L->out_ch * N);
                    if (!no_i) { free(acc); goto done; }
                    bias_requant_i8(acc, L->out_ch, N, (const int *)L->b,
                                    a_scale * L->w_scale / L->out_scale, no_i);
                    free(ai); ai = no_i; a_scale = L->out_scale;
                } else {
                    no_f = (float *)malloc((size_t)L->out_ch * N * sizeof(float));
                    if (!no_f) { free(acc); goto done; }
                    bias_dequant_f32(acc, L->out_ch, N, (const int *)L->b,
                                     a_scale * L->w_scale, no_f);
                    free(ai); ai = NULL; af = no_f; a_scale = 0.0f;
                }
                free(acc);
            } else {
                float *col = (float *)malloc((size_t)K * N * sizeof(float));
                float *out = (float *)malloc((size_t)L->out_ch * N * sizeof(float));
                if (!col || !out) { free(col); free(out); goto done; }
                im2col_f32(af, C, H, W, L->k, L->stride, L->pad, col, oH, oW);
                g_kernels.gemm_f32(L->out_ch, N, K, (const float *)L->w, col,
                                   out, 1);
                bias_add_f32(out, L->out_ch, N, (const float *)L->b);
                free(col);
                free(af); af = out;
            }
            C = L->out_ch; H = oH; W = oW; n_elem = C * H * W;
            break;
        }
        case OP_DENSE: {
            int K = L->in_n, N = 1;
            if (L->is_i8) {
                int *acc;
                if (!ai) {
                    int i;
                    ai = (signed char *)malloc((size_t)n_elem);
                    if (!ai) goto done;
                    for (i = 0; i < n_elem; i++)
                        ai[i] = ri_quant_clamp(af[i] / L->in_scale);
                    free(af); af = NULL;
                    a_scale = L->in_scale;
                }
                acc = (int *)malloc((size_t)L->out_n * sizeof(int));
                if (!acc) goto done;
                g_kernels.gemm_i8(L->out_n, N, K, (const signed char *)L->w,
                                  ai, acc, 1);
                if (L->out_scale > 0.0f) {
                    signed char *no = (signed char *)malloc((size_t)L->out_n);
                    if (!no) { free(acc); goto done; }
                    bias_requant_i8(acc, L->out_n, 1, (const int *)L->b,
                                    a_scale * L->w_scale / L->out_scale, no);
                    free(ai); ai = no; a_scale = L->out_scale;
                } else {
                    float *no = (float *)malloc((size_t)L->out_n * sizeof(float));
                    if (!no) { free(acc); goto done; }
                    bias_dequant_f32(acc, L->out_n, 1, (const int *)L->b,
                                     a_scale * L->w_scale, no);
                    free(ai); ai = NULL; af = no; a_scale = 0.0f;
                }
                free(acc);
            } else {
                float *out = (float *)malloc((size_t)L->out_n * sizeof(float));
                if (!out) goto done;
                g_kernels.gemm_f32(L->out_n, N, K, (const float *)L->w, af,
                                   out, 1);
                bias_add_f32(out, L->out_n, 1, (const float *)L->b);
                free(af); af = out;
            }
            C = L->out_n; H = 1; W = 1; n_elem = C;
            break;
        }
        case OP_RELU:
            if (ai)
                relu_i8(ai, (size_t)n_elem);
            else
                relu_f32(af, (size_t)n_elem);
            break;
        case OP_MAXPOOL: {
            int oH = (H - L->k) / L->stride + 1;
            int oW = (W - L->k) / L->stride + 1;
            if (ai) {
                signed char *out = (signed char *)malloc((size_t)C * oH * oW);
                if (!out) goto done;
                maxpool_i8(ai, C, H, W, L->k, L->stride, out, oH, oW);
                free(ai); ai = out;
            } else {
                float *out = (float *)malloc((size_t)C * oH * oW * sizeof(float));
                if (!out) goto done;
                maxpool_f32(af, C, H, W, L->k, L->stride, out, oH, oW);
                free(af); af = out;
            }
            H = oH; W = oW; n_elem = C * H * W;
            break;
        }
        case OP_FLATTEN:
            C = n_elem; H = 1; W = 1;
            break;
        case OP_SOFTMAX:
            if (ai) {              /* dequantize first */
                int i;
                af = (float *)malloc((size_t)n_elem * sizeof(float));
                if (!af) goto done;
                for (i = 0; i < n_elem; i++)
                    af[i] = (float)ai[i] * a_scale;
                free(ai); ai = NULL;
            }
            softmax_f32(af, (size_t)n_elem);
            break;
        case OP_KNN:
            goto done;             /* only valid as sole first layer */
        }
    }

    /* emit logits as f32 */
    if (ai) {
        int i;
        for (i = 0; i < m->n_classes && i < n_elem; i++)
            logits[i] = (float)ai[i] * a_scale;
    } else if (af) {
        memcpy(logits, af,
               (size_t)(m->n_classes < n_elem ? m->n_classes : n_elem) *
                   sizeof(float));
    } else {
        goto done;
    }
    ret = 0;
done:
    free(af);
    free(ai);
    return ret;
}

void model_close(model_t *m)
{
    if (!m)
        return;
    json_free(m->root);
    rim_free(&m->rim);
    free(m);
}
