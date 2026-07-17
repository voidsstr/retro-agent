#include <math.h>
#include <stdlib.h>
#include "tutil.h"

void rng_seed(rng_t *r, unsigned seed)
{
    r->s = seed ? seed : 0xBADC0DE1u;
}

unsigned rng_u32(rng_t *r)
{
    unsigned x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

float rng_uniform(rng_t *r)
{
    return (float)(rng_u32(r) >> 8) / 16777216.0f;
}

float rng_normal(rng_t *r)
{
    /* Box-Muller; discard the second value for determinism-simplicity */
    float u1 = rng_uniform(r), u2 = rng_uniform(r);
    if (u1 < 1e-7f)
        u1 = 1e-7f;
    return (float)(sqrt(-2.0 * log((double)u1)) *
                   cos(2.0 * 3.14159265358979323846 * (double)u2));
}

void rng_shuffle(rng_t *r, int *idx, int n)
{
    int i;
    for (i = n - 1; i > 0; i--) {
        int j = (int)(rng_u32(r) % (unsigned)(i + 1));
        int t = idx[i];
        idx[i] = idx[j];
        idx[j] = t;
    }
}

double metric_logloss(const float *probs, const unsigned char *labels, int n,
                      int n_classes)
{
    double s = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        double p = (double)probs[(size_t)i * n_classes + labels[i]];
        if (p < 1e-12)
            p = 1e-12;
        s -= log(p);
    }
    return s / (double)n;
}

double metric_accuracy(const float *probs, const unsigned char *labels, int n,
                       int n_classes)
{
    int i, c, correct = 0;
    for (i = 0; i < n; i++) {
        int best = 0;
        const float *p = probs + (size_t)i * n_classes;
        for (c = 1; c < n_classes; c++)
            if (p[c] > p[best])
                best = c;
        if (best == labels[i])
            correct++;
    }
    return (double)correct / (double)n;
}

typedef struct {
    float score;
    unsigned char label;
} auc_pair_t;

static int auc_cmp(const void *a, const void *b)
{
    float d = ((const auc_pair_t *)a)->score - ((const auc_pair_t *)b)->score;
    return (d > 0) - (d < 0);
}

double metric_auc(const float *scores, const unsigned char *labels, int n)
{
    auc_pair_t *p = (auc_pair_t *)malloc((size_t)n * sizeof(auc_pair_t));
    long n_pos = 0, n_neg = 0;
    double rank_sum = 0.0;
    int i, j;
    if (!p)
        return -1.0;
    for (i = 0; i < n; i++) {
        p[i].score = scores[i];
        p[i].label = labels[i] ? 1 : 0;
        if (p[i].label)
            n_pos++;
        else
            n_neg++;
    }
    if (!n_pos || !n_neg) {
        free(p);
        return -1.0;
    }
    qsort(p, (size_t)n, sizeof(auc_pair_t), auc_cmp);
    /* average ranks over score ties */
    i = 0;
    while (i < n) {
        double avg_rank;
        j = i;
        while (j + 1 < n && p[j + 1].score == p[i].score)
            j++;
        avg_rank = ((double)i + (double)j) / 2.0 + 1.0; /* 1-based */
        for (; i <= j; i++)
            if (p[i].label)
                rank_sum += avg_rank;
    }
    free(p);
    return (rank_sum - (double)n_pos * ((double)n_pos + 1.0) / 2.0) /
           ((double)n_pos * (double)n_neg);
}

void metric_prf(const float *scores, const unsigned char *labels, int n,
                double *precision, double *recall, double *f1)
{
    long tp = 0, fp = 0, fn = 0;
    int i;
    for (i = 0; i < n; i++) {
        int pred = scores[i] >= 0.5f;
        int y = labels[i] ? 1 : 0;
        if (pred && y)
            tp++;
        else if (pred && !y)
            fp++;
        else if (!pred && y)
            fn++;
    }
    *precision = tp + fp ? (double)tp / (double)(tp + fp) : 0.0;
    *recall = tp + fn ? (double)tp / (double)(tp + fn) : 0.0;
    *f1 = (*precision + *recall > 0.0)
              ? 2.0 * *precision * *recall / (*precision + *recall)
              : 0.0;
}

double metric_rmse(const float *pred, const float *target, int n)
{
    double s = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        double d = (double)pred[i] - (double)target[i];
        s += d * d;
    }
    return sqrt(s / (double)n);
}

double metric_logloss_binary(const float *scores, const unsigned char *labels,
                             int n)
{
    double s = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        double p = labels[i] ? (double)scores[i] : 1.0 - (double)scores[i];
        if (p < 1e-12)
            p = 1e-12;
        s -= log(p);
    }
    return s / (double)n;
}
