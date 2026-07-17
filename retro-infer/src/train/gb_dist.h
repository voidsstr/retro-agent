#ifndef RETRO_INFER_GB_DIST_H
#define RETRO_INFER_GB_DIST_H
#include <stddef.h>
int gbd_init(int n, int f, const unsigned char *payload, size_t plen);
void gbd_free(void);
long gbd_sumy(int *n_out);
int gbd_start(float base);
int gbd_newtree(void);
int gbd_hist(const int *frontier, int nf, unsigned char *out);
int gbd_split(const int *dec, int nd);
int gbd_leaf(const unsigned char *payload, int nl, float lr);
int gbd_nfeat(void);
int gbd_ready(void);
#endif
