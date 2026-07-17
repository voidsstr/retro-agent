#ifndef RETRO_INFER_NN_SESSION_H
#define RETRO_INFER_NN_SESSION_H

#include <stddef.h>

typedef struct nn_session nn_session_t;

nn_session_t *nns_create(const char *arch, unsigned seed, float lr,
                         float momentum);
void nns_free(nn_session_t *s);
size_t nns_n_params(const nn_session_t *s);
int nns_input_dim(const nn_session_t *s);
double nns_step(nn_session_t *s, const unsigned char *X,
                const unsigned char *y, int B, float *grads_out);
void nns_apply(nn_session_t *s, const float *grads);
int nns_eval(nn_session_t *s, const unsigned char *X, const unsigned char *y,
             int n, double *loss_out);
int nns_export(nn_session_t *s, const char *path);

#endif
