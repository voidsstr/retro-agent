#ifndef RETRO_INFER_TUTIL_H
#define RETRO_INFER_TUTIL_H

#include <stddef.h>

/* Deterministic PRNG (xorshift32) — fixed-seed reproducibility is an M2
 * acceptance test; never swap in rand(). */
typedef struct {
    unsigned s;
} rng_t;

void rng_seed(rng_t *r, unsigned seed);
unsigned rng_u32(rng_t *r);
float rng_uniform(rng_t *r);                 /* [0,1) */
float rng_normal(rng_t *r);                  /* Box-Muller, deterministic */
void rng_shuffle(rng_t *r, int *idx, int n); /* Fisher-Yates */

/* ---- metrics ---- */
double metric_logloss(const float *probs, const unsigned char *labels, int n,
                      int n_classes);
double metric_accuracy(const float *probs, const unsigned char *labels, int n,
                       int n_classes);
/* binary AUC-ROC from scores (rank-based, ties averaged) */
double metric_auc(const float *scores, const unsigned char *labels, int n);
/* binary precision/recall/F1 at 0.5 threshold on scores in [0,1] */
void metric_prf(const float *scores, const unsigned char *labels, int n,
                double *precision, double *recall, double *f1);
double metric_rmse(const float *pred, const float *target, int n);

#endif
