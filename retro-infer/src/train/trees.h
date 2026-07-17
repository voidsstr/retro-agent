#ifndef RETRO_INFER_TREES_H
#define RETRO_INFER_TREES_H

#include "tutil.h"

/* Histogram decision trees over u8-binned features (X: u8 [N,F] row-major).
 * Thresholds are bin indices: go left iff x[feat] <= thresh. */

typedef struct {
    int feat;
    int thresh;
    int left, right;    /* node indices; -1 for leaf */
    float leaf;         /* leaf value (grad trees: value; gini trees: p1) */
} tnode_t;

typedef struct {
    tnode_t *nodes;
    int n_nodes, cap;
} tree_t;

int tree_init(tree_t *t, int cap);
void tree_free(tree_t *t);
float tree_predict(const tree_t *t, const unsigned char *x);

/* Gradient tree (XGBoost-style): fit to grad g / hess h over samples idx.
 * leaf = -G/(H+lambda). Returns 0 on success. */
int tree_grow_grad(tree_t *t, const unsigned char *X, int F,
                   const float *g, const float *h, int *idx, int n,
                   int max_depth, int min_child, float lambda);

/* Gini classification tree for random forest: labels y (0/1), per-node
 * feature subsample of n_sub features drawn with rng. leaf = p(y=1). */
int tree_grow_gini(tree_t *t, const unsigned char *X, int F,
                   const unsigned char *y, int *idx, int n,
                   int max_depth, int min_child, int n_sub, rng_t *rng);

#endif
