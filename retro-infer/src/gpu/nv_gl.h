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

#endif
