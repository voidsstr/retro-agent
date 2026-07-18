#ifndef RETRO_INFER_NV_GL_H
#define RETRO_INFER_NV_GL_H

#include <stddef.h>

/* NVIDIA GL backend (M6). Compile-verified; hardware acceptance pending a
 * GeForce box coming online. Same bgemm contract as glide_mac.h. */

int nvgl_available(void);
int nvgl_init(char *err, size_t errlen);
void nvgl_shutdown(void);
int nvgl_bgemm(int M, int N, int K, const unsigned char *A,
               const unsigned char *B, int *C_matches, char *err,
               size_t errlen);


/* --nv-check acceptance driver (in glide_check.c) */
int nv_check(int M, int N, int K, unsigned seed);
/* --nv-check-multi: repeated varying-size calls in one session (debug) */
int nv_check_multi(unsigned seed);

#endif
