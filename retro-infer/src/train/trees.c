#include <stdlib.h>
#include <string.h>
#include "trees.h"

int tree_init(tree_t *t, int cap)
{
    t->nodes = (tnode_t *)malloc((size_t)cap * sizeof(tnode_t));
    t->n_nodes = 0;
    t->cap = cap;
    return t->nodes ? 0 : 1;
}

void tree_free(tree_t *t)
{
    free(t->nodes);
    t->nodes = NULL;
    t->n_nodes = t->cap = 0;
}

static int tree_add(tree_t *t)
{
    if (t->n_nodes >= t->cap)
        return -1;
    memset(&t->nodes[t->n_nodes], 0, sizeof(tnode_t));
    t->nodes[t->n_nodes].left = t->nodes[t->n_nodes].right = -1;
    return t->n_nodes++;
}

float tree_predict(const tree_t *t, const unsigned char *x)
{
    int n = 0;
    while (t->nodes[n].left >= 0)
        n = (x[t->nodes[n].feat] <= t->nodes[n].thresh) ? t->nodes[n].left
                                                        : t->nodes[n].right;
    return t->nodes[n].leaf;
}

/* stable partition of idx by (x[feat] <= thresh); returns left count */
static int partition_idx(const unsigned char *X, int F, int feat, int thresh,
                         int *idx, int n, int *scratch)
{
    int i, nl = 0, nr = 0;
    for (i = 0; i < n; i++) {
        if (X[(size_t)idx[i] * F + feat] <= thresh)
            idx[nl++] = idx[i];
        else
            scratch[nr++] = idx[i];
    }
    memcpy(idx + nl, scratch, (size_t)nr * sizeof(int));
    return nl;
}

/* ---- gradient tree ---- */

typedef struct {
    double g, h;
    int cnt;
} gh_bin_t;

static int grow_grad_rec(tree_t *t, const unsigned char *X, int F,
                         const float *g, const float *h, int *idx, int n,
                         int depth, int max_depth, int min_child,
                         float lambda, int *scratch, gh_bin_t *bins)
{
    int node = tree_add(t);
    double G = 0.0, H = 0.0;
    int i, f;
    int best_f = -1, best_t = 0;
    double best_gain = 0.0;

    if (node < 0)
        return -1;
    for (i = 0; i < n; i++) {
        G += (double)g[idx[i]];
        H += (double)h[idx[i]];
    }

    if (depth < max_depth && n >= 2 * min_child) {
        double parent = G * G / (H + (double)lambda);
        for (f = 0; f < F; f++) {
            double GL = 0.0, HL = 0.0;
            int CL = 0, b;
            memset(bins, 0, 256 * sizeof(gh_bin_t));
            for (i = 0; i < n; i++) {
                unsigned char v = X[(size_t)idx[i] * F + f];
                bins[v].g += (double)g[idx[i]];
                bins[v].h += (double)h[idx[i]];
                bins[v].cnt++;
            }
            for (b = 0; b < 255; b++) {   /* split at bin b: left <= b */
                double GR, HR, gain;
                GL += bins[b].g;
                HL += bins[b].h;
                CL += bins[b].cnt;
                if (CL < min_child)
                    continue;
                if (n - CL < min_child)
                    break;
                GR = G - GL;
                HR = H - HL;
                gain = GL * GL / (HL + (double)lambda) +
                       GR * GR / (HR + (double)lambda) - parent;
                if (gain > best_gain) {
                    best_gain = gain;
                    best_f = f;
                    best_t = b;
                }
            }
        }
    }

    if (best_f < 0) {
        t->nodes[node].leaf = (float)(-G / (H + (double)lambda));
        return node;
    }
    {
        int nl = partition_idx(X, F, best_f, best_t, idx, n, scratch);
        int lch, rch;
        t->nodes[node].feat = best_f;
        t->nodes[node].thresh = best_t;
        lch = grow_grad_rec(t, X, F, g, h, idx, nl, depth + 1, max_depth,
                            min_child, lambda, scratch, bins);
        rch = grow_grad_rec(t, X, F, g, h, idx + nl, n - nl, depth + 1,
                            max_depth, min_child, lambda, scratch, bins);
        if (lch < 0 || rch < 0)
            return -1;
        t->nodes[node].left = lch;
        t->nodes[node].right = rch;
    }
    return node;
}

int tree_grow_grad(tree_t *t, const unsigned char *X, int F,
                   const float *g, const float *h, int *idx, int n,
                   int max_depth, int min_child, float lambda)
{
    int *scratch = (int *)malloc((size_t)n * sizeof(int));
    gh_bin_t *bins = (gh_bin_t *)malloc(256 * sizeof(gh_bin_t));
    int r;
    if (!scratch || !bins) {
        free(scratch);
        free(bins);
        return 1;
    }
    t->n_nodes = 0;
    r = grow_grad_rec(t, X, F, g, h, idx, n, 0, max_depth, min_child, lambda,
                      scratch, bins);
    free(scratch);
    free(bins);
    return r < 0;
}

/* ---- gini tree (random forest) ---- */

static int grow_gini_rec(tree_t *t, const unsigned char *X, int F,
                         const unsigned char *y, int *idx, int n,
                         int depth, int max_depth, int min_child, int n_sub,
                         rng_t *rng, int *scratch, int *feats, int *cbins)
{
    int node = tree_add(t);
    int n1 = 0, i, fi;
    int best_f = -1, best_t = 0;
    double best_gini = 1e30;

    if (node < 0)
        return -1;
    for (i = 0; i < n; i++)
        n1 += y[idx[i]];

    if (depth < max_depth && n >= 2 * min_child && n1 > 0 && n1 < n) {
        /* per-node random feature subset (Fisher-Yates prefix) */
        for (i = 0; i < F; i++)
            feats[i] = i;
        for (i = 0; i < n_sub && i < F; i++) {
            int j = i + (int)(rng_u32(rng) % (unsigned)(F - i));
            int tmp = feats[i];
            feats[i] = feats[j];
            feats[j] = tmp;
        }
        for (fi = 0; fi < n_sub && fi < F; fi++) {
            int f = feats[fi], b;
            int CL = 0, CL1 = 0;
            memset(cbins, 0, 256 * 2 * sizeof(int));
            for (i = 0; i < n; i++) {
                unsigned char v = X[(size_t)idx[i] * F + f];
                cbins[v * 2]++;
                cbins[v * 2 + 1] += y[idx[i]];
            }
            for (b = 0; b < 255; b++) {
                double pl, pr, gl, gr, wg;
                int CR, CR1;
                CL += cbins[b * 2];
                CL1 += cbins[b * 2 + 1];
                if (CL < min_child)
                    continue;
                CR = n - CL;
                CR1 = n1 - CL1;
                if (CR < min_child)
                    break;
                pl = (double)CL1 / (double)CL;
                pr = (double)CR1 / (double)CR;
                gl = 2.0 * pl * (1.0 - pl);
                gr = 2.0 * pr * (1.0 - pr);
                wg = ((double)CL * gl + (double)CR * gr) / (double)n;
                if (wg < best_gini) {
                    best_gini = wg;
                    best_f = f;
                    best_t = b;
                }
            }
        }
    }

    if (best_f < 0) {
        t->nodes[node].leaf = (float)((double)n1 / (double)n);
        return node;
    }
    {
        int nl = partition_idx(X, F, best_f, best_t, idx, n, scratch);
        int lch, rch;
        t->nodes[node].feat = best_f;
        t->nodes[node].thresh = best_t;
        lch = grow_gini_rec(t, X, F, y, idx, nl, depth + 1, max_depth,
                            min_child, n_sub, rng, scratch, feats, cbins);
        rch = grow_gini_rec(t, X, F, y, idx + nl, n - nl, depth + 1,
                            max_depth, min_child, n_sub, rng, scratch, feats,
                            cbins);
        if (lch < 0 || rch < 0)
            return -1;
        t->nodes[node].left = lch;
        t->nodes[node].right = rch;
    }
    return node;
}

int tree_grow_gini(tree_t *t, const unsigned char *X, int F,
                   const unsigned char *y, int *idx, int n,
                   int max_depth, int min_child, int n_sub, rng_t *rng)
{
    int *scratch = (int *)malloc((size_t)n * sizeof(int));
    int *feats = (int *)malloc((size_t)F * sizeof(int));
    int *cbins = (int *)malloc(256 * 2 * sizeof(int));
    int r;
    if (!scratch || !feats || !cbins) {
        free(scratch);
        free(feats);
        free(cbins);
        return 1;
    }
    t->n_nodes = 0;
    r = grow_gini_rec(t, X, F, y, idx, n, 0, max_depth, min_child, n_sub, rng,
                      scratch, feats, cbins);
    free(scratch);
    free(feats);
    free(cbins);
    return r < 0;
}
