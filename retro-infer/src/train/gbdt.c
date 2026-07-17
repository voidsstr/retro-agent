/*
 * From-scratch histogram GBDT (M2): binary logistic + regression, u8-binned
 * features, XGBoost-style leaf values, deterministic (no sampling).
 *
 * Data: features u8 [N,F]; labels u8 (classify) or f32 LE (regress).
 * Split rule matches tools/rim/tabular: first (1-val_frac) train, rest val
 * (files are pre-permuted on the dev side).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../port.h"
#include "tutil.h"
#include "trees.h"
#include "gbdt.h"

static unsigned char *read_all(const char *path, size_t need)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    if (!f)
        return NULL;
    buf = (unsigned char *)malloc(need);
    if (!buf || fread(buf, 1, need, f) != need) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    return buf;
}

int gbdt_run(const gbdt_cfg_t *cfg)
{
    unsigned char *X = NULL, *yb = NULL;
    float *yr = NULL;
    float *Ftr = NULL, *Fva = NULL, *g = NULL, *h = NULL;
    float *val_scores = NULL;
    unsigned char *val_labels = NULL;
    int *idx = NULL;
    tree_t *trees = NULL;
    int n_tr, n_va, i, r, ret = 1;
    double base, t0;

    X = read_all(cfg->features, (size_t)cfg->n * cfg->f);
    if (!X) {
        printf("gbdt: FAIL: cannot read features\n");
        goto done;
    }
    if (cfg->regress) {
        yr = (float *)read_all(cfg->labels, (size_t)cfg->n * 4);
        if (!yr) {
            printf("gbdt: FAIL: cannot read targets\n");
            goto done;
        }
    } else {
        yb = read_all(cfg->labels, (size_t)cfg->n);
        if (!yb) {
            printf("gbdt: FAIL: cannot read labels\n");
            goto done;
        }
    }

    n_va = (int)((double)cfg->n * (double)cfg->val_frac);
    n_tr = cfg->n - n_va;

    Ftr = (float *)malloc((size_t)n_tr * sizeof(float));
    Fva = (float *)malloc((size_t)n_va * sizeof(float));
    g = (float *)malloc((size_t)n_tr * sizeof(float));
    h = (float *)malloc((size_t)n_tr * sizeof(float));
    idx = (int *)malloc((size_t)n_tr * sizeof(int));
    trees = (tree_t *)calloc((size_t)cfg->rounds, sizeof(tree_t));
    val_scores = (float *)malloc((size_t)n_va * sizeof(float));
    val_labels = (unsigned char *)malloc((size_t)n_va);
    if (!Ftr || !Fva || !g || !h || !idx || !trees || !val_scores ||
        !val_labels) {
        printf("gbdt: FAIL: out of memory\n");
        goto done;
    }

    /* base score */
    if (cfg->regress) {
        double s = 0.0;
        for (i = 0; i < n_tr; i++)
            s += (double)yr[i];
        base = s / (double)n_tr;
    } else {
        long pos = 0;
        for (i = 0; i < n_tr; i++)
            pos += yb[i];
        {
            double p = (double)pos / (double)n_tr;
            if (p < 1e-6) p = 1e-6;
            if (p > 1.0 - 1e-6) p = 1.0 - 1e-6;
            base = log(p / (1.0 - p));
        }
    }
    for (i = 0; i < n_tr; i++)
        Ftr[i] = (float)base;
    for (i = 0; i < n_va; i++)
        Fva[i] = (float)base;

    printf("gbdt.n_train=%d gbdt.n_val=%d gbdt.f=%d rounds=%d depth=%d "
           "lr=%g lambda=%g min_child=%d task=%s\n",
           n_tr, n_va, cfg->f, cfg->rounds, cfg->depth, (double)cfg->lr,
           (double)cfg->lambda, cfg->min_child,
           cfg->regress ? "regress" : "classify");
    t0 = ri_now();

    for (r = 0; r < cfg->rounds; r++) {
        int cap = 1 << (cfg->depth + 1);
        if (tree_init(&trees[r], cap * 2)) {
            printf("gbdt: FAIL: oom tree\n");
            goto done;
        }
        for (i = 0; i < n_tr; i++) {
            if (cfg->regress) {
                g[i] = Ftr[i] - yr[i];
                h[i] = 1.0f;
            } else {
                float p = (float)(1.0 / (1.0 + exp(-(double)Ftr[i])));
                g[i] = p - (float)yb[i];
                h[i] = p * (1.0f - p) + 1e-6f;
            }
            idx[i] = i;
        }
        if (tree_grow_grad(&trees[r], X, cfg->f, g, h, idx, n_tr, cfg->depth,
                           cfg->min_child, cfg->lambda)) {
            printf("gbdt: FAIL: tree grow\n");
            goto done;
        }
        for (i = 0; i < n_tr; i++)
            Ftr[i] += cfg->lr * tree_predict(&trees[r], X + (size_t)i * cfg->f);
        for (i = 0; i < n_va; i++)
            Fva[i] += cfg->lr *
                      tree_predict(&trees[r],
                                   X + (size_t)(n_tr + i) * cfg->f);

        if ((r + 1) % 10 == 0 || r == cfg->rounds - 1) {
            if (cfg->regress) {
                double str = 0.0, sva = 0.0;
                for (i = 0; i < n_tr; i++) {
                    double d = (double)Ftr[i] - yr[i];
                    str += d * d;
                }
                for (i = 0; i < n_va; i++) {
                    double d = (double)Fva[i] - yr[n_tr + i];
                    sva += d * d;
                }
                printf("round=%d train_rmse=%.5f val_rmse=%.5f elapsed=%.1fs\n",
                       r + 1, sqrt(str / n_tr), sqrt(sva / n_va),
                       ri_now() - t0);
            } else {
                double ltr = 0.0, lva = 0.0;
                for (i = 0; i < n_tr; i++) {
                    double p = 1.0 / (1.0 + exp(-(double)Ftr[i]));
                    p = yb[i] ? p : 1.0 - p;
                    if (p < 1e-12) p = 1e-12;
                    ltr -= log(p);
                }
                for (i = 0; i < n_va; i++) {
                    double p = 1.0 / (1.0 + exp(-(double)Fva[i]));
                    p = yb[n_tr + i] ? p : 1.0 - p;
                    if (p < 1e-12) p = 1e-12;
                    lva -= log(p);
                }
                printf("round=%d train_logloss=%.5f val_logloss=%.5f "
                       "elapsed=%.1fs\n",
                       r + 1, ltr / n_tr, lva / n_va, ri_now() - t0);
            }
            fflush(stdout);
        }
    }

    /* final val metrics */
    if (cfg->regress) {
        double s = 0.0;
        for (i = 0; i < n_va; i++) {
            double d = (double)Fva[i] - yr[n_tr + i];
            s += d * d;
        }
        printf("gbdt.val_rmse=%.5f\n", sqrt(s / n_va));
    } else {
        double auc;
        long correct = 0;
        for (i = 0; i < n_va; i++) {
            val_scores[i] = (float)(1.0 / (1.0 + exp(-(double)Fva[i])));
            val_labels[i] = yb[n_tr + i];
            if ((val_scores[i] >= 0.5f) == (val_labels[i] != 0))
                correct++;
        }
        auc = metric_auc(val_scores, val_labels, n_va);
        printf("gbdt.val_auc=%.5f gbdt.val_acc=%.5f gbdt.val_logloss=%.5f\n",
               auc, (double)correct / n_va,
               metric_logloss_binary(val_scores, val_labels, n_va));
    }
    printf("gbdt.total_secs=%.1f gbdt.trees_per_sec=%.2f\n", ri_now() - t0,
           (double)cfg->rounds / (ri_now() - t0));
    printf("gbdt: OK\n");
    ret = 0;
done:
    if (trees) {
        for (r = 0; r < cfg->rounds; r++)
            tree_free(&trees[r]);
        free(trees);
    }
    free(X); free(yb); free(yr);
    free(Ftr); free(Fva); free(g); free(h); free(idx);
    free(val_scores); free(val_labels);
    return ret;
}
