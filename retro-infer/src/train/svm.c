/*
 * Linear SVM (M2): Pegasos-style SGD on hinge loss, features u8/255,
 * deterministic shuffle. Binary labels {0,1} -> {-1,+1}.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../port.h"
#include "tutil.h"
#include "gbdt.h"

int svm_run(const svm_cfg_t *cfg)
{
    unsigned char *X = NULL, *y = NULL;
    float *w = NULL, *val_scores = NULL;
    int *order = NULL;
    rng_t rng;
    int n_tr, n_va, i, j, e, ret = 1;
    float bias = 0.0f;
    double t0;
    FILE *f;

    f = fopen(cfg->features, "rb");
    X = (unsigned char *)malloc((size_t)cfg->n * cfg->f);
    if (!f || !X || fread(X, 1, (size_t)cfg->n * cfg->f, f) !=
                        (size_t)cfg->n * cfg->f) {
        printf("svm: FAIL: cannot read features\n");
        if (f) fclose(f);
        goto done;
    }
    fclose(f);
    f = fopen(cfg->labels, "rb");
    y = (unsigned char *)malloc((size_t)cfg->n);
    if (!f || !y || fread(y, 1, (size_t)cfg->n, f) != (size_t)cfg->n) {
        printf("svm: FAIL: cannot read labels\n");
        if (f) fclose(f);
        goto done;
    }
    fclose(f);

    n_va = (int)((double)cfg->n * (double)cfg->val_frac);
    n_tr = cfg->n - n_va;
    w = (float *)calloc((size_t)cfg->f, sizeof(float));
    order = (int *)malloc((size_t)n_tr * sizeof(int));
    val_scores = (float *)malloc((size_t)n_va * sizeof(float));
    if (!w || !order || !val_scores) {
        printf("svm: FAIL: out of memory\n");
        goto done;
    }
    for (i = 0; i < n_tr; i++)
        order[i] = i;
    rng_seed(&rng, (unsigned)cfg->seed);

    printf("svm.n_train=%d n_val=%d f=%d epochs=%d lr=%g reg=%g seed=%d\n",
           n_tr, n_va, cfg->f, cfg->epochs, (double)cfg->lr,
           (double)cfg->reg, cfg->seed);
    t0 = ri_now();

    for (e = 0; e < cfg->epochs; e++) {
        double hinge = 0.0;
        rng_shuffle(&rng, order, n_tr);
        for (i = 0; i < n_tr; i++) {
            const unsigned char *xi = X + (size_t)order[i] * cfg->f;
            float yy = y[order[i]] ? 1.0f : -1.0f;
            float m = bias;
            for (j = 0; j < cfg->f; j++)
                m += w[j] * ((float)xi[j] / 255.0f);
            m *= yy;
            if (m < 1.0f)
                hinge += 1.0 - (double)m;
            /* w <- w - lr*(reg*w - 1[margin<1]*y*x) */
            for (j = 0; j < cfg->f; j++) {
                float grad = cfg->reg * w[j];
                if (m < 1.0f)
                    grad -= yy * ((float)xi[j] / 255.0f);
                w[j] -= cfg->lr * grad;
            }
            if (m < 1.0f)
                bias += cfg->lr * yy;
        }
        printf("epoch=%d train_hinge=%.5f elapsed=%.1fs\n", e + 1,
               hinge / n_tr, ri_now() - t0);
        fflush(stdout);
    }

    {
        long correct = 0;
        double prec, rec, f1;
        for (i = 0; i < n_va; i++) {
            const unsigned char *xi = X + (size_t)(n_tr + i) * cfg->f;
            float m = bias;
            for (j = 0; j < cfg->f; j++)
                m += w[j] * ((float)xi[j] / 255.0f);
            val_scores[i] = m >= 0.0f ? 1.0f : 0.0f;
            if ((m >= 0.0f) == (y[n_tr + i] != 0))
                correct++;
        }
        metric_prf(val_scores, y + n_tr, n_va, &prec, &rec, &f1);
        printf("svm.val_acc=%.5f svm.val_precision=%.5f svm.val_recall=%.5f "
               "svm.val_f1=%.5f\n",
               (double)correct / n_va, prec, rec, f1);
    }
    printf("svm.total_secs=%.1f\n", ri_now() - t0);
    printf("svm: OK\n");
    ret = 0;
done:
    free(X); free(y); free(w); free(order); free(val_scores);
    return ret;
}
