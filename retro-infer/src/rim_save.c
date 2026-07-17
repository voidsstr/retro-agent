/*
 * Minimal C-side .rim writer for dense-f32 models produced by the on-device
 * trainer (tools/rim/FORMAT.md). Layout mirrors rim_pack.py: RIM1 header,
 * manifest JSON, pad to 16, weights blob with 16-byte-aligned tensors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "infer.h"
#include "train/train_nn.h"

static void wr_u32le(FILE *f, unsigned v)
{
    unsigned char b[4];
    b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF;
    b[2] = (v >> 16) & 0xFF; b[3] = (v >> 24) & 0xFF;
    fwrite(b, 1, 4, f);
}

static size_t align16(size_t x)
{
    return (x + 15u) & ~(size_t)15u;
}

int rim_save_dense(const char *path, const int *dims, int n_layers,
                   const float *const *W, const float *const *b,
                   int input_u8_div255)
{
    char *manifest;
    size_t mcap = 4096, mlen = 0, off = 0, pos, i;
    size_t *w_off, *b_off;
    FILE *f;
    int l;

    manifest = (char *)malloc(mcap);
    w_off = (size_t *)malloc((size_t)n_layers * sizeof(size_t));
    b_off = (size_t *)malloc((size_t)n_layers * sizeof(size_t));
    if (!manifest || !w_off || !b_off) {
        free(manifest); free(w_off); free(b_off);
        return 1;
    }

    /* assign blob offsets */
    for (l = 0; l < n_layers; l++) {
        w_off[l] = off;
        off = align16(off + (size_t)dims[l] * dims[l + 1] * 4);
        b_off[l] = off;
        off = align16(off + (size_t)dims[l + 1] * 4);
    }

    mlen += (size_t)sprintf(manifest + mlen,
        "{\"rim\":1,\"name\":\"trained-mlp\",\"input\":{\"shape\":[%d],"
        "\"dtype\":\"%s\"%s},\"layers\":[",
        dims[0], input_u8_div255 ? "u8" : "f32",
        input_u8_div255 ? ",\"div\":255.0" : "");
    for (l = 0; l < n_layers; l++) {
        if (mlen + 512 > mcap) {
            char *nm = (char *)realloc(manifest, mcap *= 2);
            if (!nm) {
                free(manifest); free(w_off); free(b_off);
                return 1;
            }
            manifest = nm;
        }
        mlen += (size_t)sprintf(manifest + mlen,
            "%s{\"op\":\"dense\",\"dtype\":\"f32\",\"in\":%d,\"out\":%d,"
            "\"w\":{\"off\":%lu,\"dtype\":\"f32\",\"shape\":[%d,%d]},"
            "\"b\":{\"off\":%lu,\"dtype\":\"f32\",\"shape\":[%d]}}",
            l ? "," : "", dims[l], dims[l + 1],
            (unsigned long)w_off[l], dims[l + 1], dims[l],
            (unsigned long)b_off[l], dims[l + 1]);
        if (l < n_layers - 1)
            mlen += (size_t)sprintf(manifest + mlen, ",{\"op\":\"relu\"}");
    }
    mlen += (size_t)sprintf(manifest + mlen, ",{\"op\":\"softmax\"}]}");

    f = fopen(path, "wb");
    if (!f) {
        free(manifest); free(w_off); free(b_off);
        return 1;
    }
    fwrite("RIM1", 1, 4, f);
    wr_u32le(f, 1);
    wr_u32le(f, (unsigned)mlen);
    fwrite(manifest, 1, mlen, f);
    pos = 12 + mlen;
    for (i = pos; i < align16(pos); i++)
        fputc(0, f);

    /* weights blob with the same alignment padding as offsets above */
    pos = 0;
    for (l = 0; l < n_layers; l++) {
        size_t wn = (size_t)dims[l] * dims[l + 1] * 4;
        size_t bn = (size_t)dims[l + 1] * 4;
        for (; pos < w_off[l]; pos++) fputc(0, f);
        fwrite(W[l], 1, wn, f); pos += wn;
        for (; pos < b_off[l]; pos++) fputc(0, f);
        fwrite(b[l], 1, bn, f); pos += bn;
    }
    fclose(f);
    free(manifest); free(w_off); free(b_off);
    return 0;
}
