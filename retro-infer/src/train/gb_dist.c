/*
 * Distributed-GBDT node side (M7): the brain shards rows across boxes and
 * coordinates tree building; each node only ever sees its own shard.
 *
 * Verbs (serve.c):
 *   GBINIT <n> <f>       payload: X u8 [n*f] + y u8 [n]
 *   GBSUMY               -> "sum=<int> n=<int>"
 *   GBSTART <base>       -> F[i] = base
 *   GBNEWTREE            -> g,h from logistic loss; assign[i] = 0
 *   GBHIST               payload: [u32 nf][i32 frontier ids]
 *                        -> per frontier node x feature x 256 bins:
 *                           (f32 sum_g, f32 sum_h, u32 cnt)
 *   GBSPLIT              payload: [u32 nd][(i32 node,f32 pad? no:
 *                        i32 node, i32 feat, i32 thresh, i32 left, i32 right)]
 *   GBLEAF <lr>          payload: [u32 nl][(i32 node, f32 value)]
 *                        -> F[i] += lr * value[assign match]
 *   GBFREE
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gb_dist.h"

static unsigned char *gb_X, *gb_y;
static float *gb_F, *gb_g, *gb_h;
static int *gb_assign;
static int gb_n, gb_f;

void gbd_free(void)
{
    free(gb_X); free(gb_y); free(gb_F); free(gb_g); free(gb_h);
    free(gb_assign);
    gb_X = gb_y = NULL;
    gb_F = gb_g = gb_h = NULL;
    gb_assign = NULL;
    gb_n = gb_f = 0;
}

int gbd_init(int n, int f, const unsigned char *payload, size_t plen)
{
    if (n <= 0 || f <= 0 || plen < (size_t)n * f + n)
        return 1;
    gbd_free();
    gb_X = (unsigned char *)malloc((size_t)n * f);
    gb_y = (unsigned char *)malloc((size_t)n);
    gb_F = (float *)malloc((size_t)n * sizeof(float));
    gb_g = (float *)malloc((size_t)n * sizeof(float));
    gb_h = (float *)malloc((size_t)n * sizeof(float));
    gb_assign = (int *)malloc((size_t)n * sizeof(int));
    if (!gb_X || !gb_y || !gb_F || !gb_g || !gb_h || !gb_assign) {
        gbd_free();
        return 1;
    }
    memcpy(gb_X, payload, (size_t)n * f);
    memcpy(gb_y, payload + (size_t)n * f, (size_t)n);
    gb_n = n;
    gb_f = f;
    return 0;
}

long gbd_sumy(int *n_out)
{
    long s = 0;
    int i;
    for (i = 0; i < gb_n; i++)
        s += gb_y[i];
    *n_out = gb_n;
    return s;
}

int gbd_start(float base)
{
    int i;
    if (!gb_F)
        return 1;
    for (i = 0; i < gb_n; i++)
        gb_F[i] = base;
    return 0;
}

int gbd_newtree(void)
{
    int i;
    if (!gb_F)
        return 1;
    for (i = 0; i < gb_n; i++) {
        float p = (float)(1.0 / (1.0 + exp(-(double)gb_F[i])));
        gb_g[i] = p - (float)gb_y[i];
        gb_h[i] = p * (1.0f - p) + 1e-6f;
        gb_assign[i] = 0;
    }
    return 0;
}

/* out must hold nf * gb_f * 256 * 12 bytes */
int gbd_hist(const int *frontier, int nf, unsigned char *out)
{
    int fi, i;
    memset(out, 0, (size_t)nf * gb_f * 256 * 12);
    for (i = 0; i < gb_n; i++) {
        int a = gb_assign[i], slot = -1;
        for (fi = 0; fi < nf; fi++)
            if (frontier[fi] == a) {
                slot = fi;
                break;
            }
        if (slot < 0)
            continue;
        {
            const unsigned char *row = gb_X + (size_t)i * gb_f;
            int f;
            for (f = 0; f < gb_f; f++) {
                unsigned char *cell = out +
                    (((size_t)slot * gb_f + f) * 256 + row[f]) * 12;
                float sg, sh;
                unsigned cnt;
                memcpy(&sg, cell, 4);
                memcpy(&sh, cell + 4, 4);
                memcpy(&cnt, cell + 8, 4);
                sg += gb_g[i];
                sh += gb_h[i];
                cnt++;
                memcpy(cell, &sg, 4);
                memcpy(cell + 4, &sh, 4);
                memcpy(cell + 8, &cnt, 4);
            }
        }
    }
    return 0;
}

int gbd_split(const int *dec, int nd)
{
    /* dec: nd x 5 ints (node, feat, thresh, left, right) */
    int i, d;
    for (i = 0; i < gb_n; i++) {
        int a = gb_assign[i];
        for (d = 0; d < nd; d++) {
            if (dec[d * 5] == a) {
                const unsigned char *row = gb_X + (size_t)i * gb_f;
                gb_assign[i] = row[dec[d * 5 + 1]] <= dec[d * 5 + 2]
                                   ? dec[d * 5 + 3]
                                   : dec[d * 5 + 4];
                break;
            }
        }
    }
    return 0;
}

int gbd_leaf(const unsigned char *payload, int nl, float lr)
{
    /* payload: nl x (i32 node, f32 value) */
    int i, d;
    for (i = 0; i < gb_n; i++) {
        int a = gb_assign[i];
        for (d = 0; d < nl; d++) {
            int node;
            float val;
            memcpy(&node, payload + (size_t)d * 8, 4);
            memcpy(&val, payload + (size_t)d * 8 + 4, 4);
            if (node == a) {
                gb_F[i] += lr * val;
                break;
            }
        }
    }
    return 0;
}

int gbd_nfeat(void) { return gb_f; }
int gbd_ready(void) { return gb_X != NULL; }
