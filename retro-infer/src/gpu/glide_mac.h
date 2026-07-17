#ifndef RETRO_INFER_GLIDE_MAC_H
#define RETRO_INFER_GLIDE_MAC_H

#include <stddef.h>

/* 3dfx Voodoo binary-GEMM backend (M5). All entry points safe to call on
 * boxes without a Voodoo (dynamic glide3x.dll binding). */

int glide_available(void);
int glide_init(char *err, size_t errlen);
void glide_shutdown(void);

/* C_matches[i,j] += #k where A[i,k]==B[k,j]; A,B one byte per bit (0/1),
 * A [M,K], B [K,N], M/N/K <= 256. Signed XNOR dot = 2*matches - K. */
int glide_bgemm(int M, int N, int K, const unsigned char *A,
                const unsigned char *B, int *C_matches, char *err,
                size_t errlen);

/* CPU reference for the same operation (always available) */
void bgemm_cpu(int M, int N, int K, const unsigned char *A,
               const unsigned char *B, int *C_matches);

/* --glide-check driver: selftest + random parity + throughput + hash */
int glide_check(int M, int N, int K, unsigned seed);

#endif
