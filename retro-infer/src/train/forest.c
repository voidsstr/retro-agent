/*
 * Random forest (M2): bootstrap-sampled gini trees, per-node sqrt(F) feature
 * subsets, majority (probability-average) vote, OOB error. Deterministic
 * with fixed seed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../port.h"
#include "tutil.h"
#include "trees.h"
#include "gbdt.h"

int forest_run(const forest_cfg_t *cfg)
{
    unsigned char *X = NULL, *y = NULL;
    tree_t *trees = NULL;
    int *idx = NULL, *oob_cnt = NULL;
    float *votes = NULL, *oob_sum = NULL, *val_scores = NULL;
    rng_t rng;
    int n_tr, n_va, n_sub, i, t, ret = 1;
    double t0;
    FILE *f;

    f = fopen(cfg->features, "rb");
    X = (unsigned char *)malloc((size_t)cfg->n * cfg->f);
    if (!f || !X || fread(X, 1, (size_t)cfg->n * cfg->f, f) !=
                        (size_t)cfg->n * cfg->f) {
        printf("forest: FAIL: cannot read features\n");
        if (f) fclose(f);
        goto done;
    }
    fclose(f);
    f = fopen(cfg->labels, "rb");
    y = (unsigned char *)malloc((size_t)cfg->n);
    if (!f || !y || fread(y, 1, (size_t)cfg->n, f) != (size_t)cfg->n) {
        printf("forest: FAIL: cannot read labels\n");
        if (f) fclose(f);
        goto done;
    }
    fclose(f);

    n_va = (int)((double)cfg->n * (double)cfg->val_frac);
    n_tr = cfg->n - n_va;
    n_sub = (int)(sqrt((double)cfg->f) + 0.5);
    if (n_sub < 1)
        n_sub = 1;

    trees = (tree_t *)calloc((size_t)cfg->n_trees, sizeof(tree_t));
    idx = (int *)malloc((size_t)n_tr * sizeof(int));
    votes = (float *)calloc((size_t)n_va, sizeof(float));
    oob_sum = (float *)calloc((size_t)n_tr, sizeof(float));
    oob_cnt = (int *)calloc((size_t)n_tr, sizeof(int));
    val_scores = (float *)malloc((size_t)n_va * sizeof(float));
    if (!trees || !idx || !votes || !oob_sum || !oob_cnt || !val_scores) {
        printf("forest: FAIL: out of memory\n");
        goto done;
    }

    rng_seed(&rng, (unsigned)cfg->seed);
    printf("forest.n_train=%d n_val=%d f=%d trees=%d depth=%d n_sub=%d "
           "seed=%d\n",
           n_tr, n_va, cfg->f, cfg->n_trees, cfg->depth, n_sub, cfg->seed);
    t0 = ri_now();

    for (t = 0; t < cfg->n_trees; t++) {
        unsigned char *in_bag = (unsigned char *)calloc((size_t)n_tr, 1);
        int cap = 1 << (cfg->depth + 1);
        if (!in_bag || tree_init(&trees[t], cap * 2)) {
            printf("forest: FAIL: oom tree\n");
            free(in_bag);
            goto done;
        }
        for (i = 0; i < n_tr; i++) {
            int s = (int)(rng_u32(&rng) % (unsigned)n_tr);
            idx[i] = s;
            in_bag[s] = 1;
        }
        if (tree_grow_gini(&trees[t], X, cfg->f, y, idx, n_tr, cfg->depth,
                           cfg->min_child, n_sub, &rng)) {
            printf("forest: FAIL: tree grow\n");
            free(in_bag);
            goto done;
        }
        /* OOB accumulation */
        for (i = 0; i < n_tr; i++) {
            if (!in_bag[i]) {
                oob_sum[i] += tree_predict(&trees[t], X + (size_t)i * cfg->f);
                oob_cnt[i]++;
            }
        }
        for (i = 0; i < n_va; i++)
            votes[i] += tree_predict(&trees[t],
                                     X + (size_t)(n_tr + i) * cfg->f);
        free(in_bag);
        if ((t + 1) % 20 == 0) {
            printf("tree=%d elapsed=%.1fs\n", t + 1, ri_now() - t0);
            fflush(stdout);
        }
    }

    {
        long oob_wrong = 0, oob_n = 0, correct = 0;
        double prec, rec, f1;
        for (i = 0; i < n_tr; i++) {
            if (oob_cnt[i]) {
                int pred = (oob_sum[i] / (float)oob_cnt[i]) >= 0.5f;
                oob_n++;
                if (pred != (y[i] != 0))
                    oob_wrong++;
            }
        }
        for (i = 0; i < n_va; i++) {
            val_scores[i] = votes[i] / (float)cfg->n_trees;
            if ((val_scores[i] >= 0.5f) == (y[n_tr + i] != 0))
                correct++;
        }
        metric_prf(val_scores, y + n_tr, n_va, &prec, &rec, &f1);
        printf("forest.oob_err=%.5f forest.val_acc=%.5f forest.val_f1=%.5f "
               "forest.val_precision=%.5f forest.val_recall=%.5f\n",
               oob_n ? (double)oob_wrong / (double)oob_n : -1.0,
               (double)correct / (double)n_va, f1, prec, rec);
        printf("forest.val_auc=%.5f\n",
               metric_auc(val_scores, y + n_tr, n_va));
    }
    printf("forest.total_secs=%.1f\n", ri_now() - t0);
    printf("forest: OK\n");
    ret = 0;
done:
    if (trees) {
        for (t = 0; t < cfg->n_trees; t++)
            tree_free(&trees[t]);
        free(trees);
    }
    free(X); free(y); free(idx); free(votes);
    free(oob_sum); free(oob_cnt); free(val_scores);
    return ret;
}
