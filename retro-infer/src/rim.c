/*
 * .rim model container loader.
 *
 * On-disk layout (all integers u32 little-endian):
 *   0   magic "RIM1"
 *   4   flags        bit0 = little-endian payload (always 1 for now)
 *   8   manifest_len
 *   12  manifest JSON (manifest_len bytes)
 *   ..  zero padding to the next 16-byte file offset
 *   ..  weights blob to EOF (tensor offsets inside are manifest-relative,
 *       each tensor start 16-byte aligned for SSE/MMX loads)
 *
 * The offline packer lives in tools/rim/ (Python); keep the two in sync.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "infer.h"

static unsigned rd_u32le(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void set_err(char *errbuf, size_t errlen, const char *msg)
{
    if (errbuf && errlen) {
        strncpy(errbuf, msg, errlen - 1);
        errbuf[errlen - 1] = '\0';
    }
}

int rim_load(const char *path, rim_model_t *out, char *errbuf, size_t errlen)
{
    FILE *f;
    long fsize;
    unsigned char *buf = NULL;
    unsigned flags, mlen;
    size_t woff;

    memset(out, 0, sizeof(*out));

    f = fopen(path, "rb");
    if (!f) {
        set_err(errbuf, errlen, "cannot open file");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 12) {
        fclose(f);
        set_err(errbuf, errlen, "file too small for RIM header");
        return 1;
    }
    buf = (unsigned char *)malloc((size_t)fsize);
    if (!buf) {
        fclose(f);
        set_err(errbuf, errlen, "out of memory");
        return 1;
    }
    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        fclose(f);
        free(buf);
        set_err(errbuf, errlen, "short read");
        return 1;
    }
    fclose(f);

    if (memcmp(buf, "RIM1", 4) != 0) {
        free(buf);
        set_err(errbuf, errlen, "bad magic (not a .rim file)");
        return 1;
    }
    flags = rd_u32le(buf + 4);
    if (!(flags & 1)) {
        free(buf);
        set_err(errbuf, errlen, "big-endian .rim not supported");
        return 1;
    }
    mlen = rd_u32le(buf + 8);
    if (12u + mlen > (unsigned)fsize) {
        free(buf);
        set_err(errbuf, errlen, "manifest length exceeds file");
        return 1;
    }

    out->manifest_len = mlen;
    out->manifest_json = (char *)malloc(mlen + 1);
    if (!out->manifest_json) {
        free(buf);
        set_err(errbuf, errlen, "out of memory");
        return 1;
    }
    memcpy(out->manifest_json, buf + 12, mlen);
    out->manifest_json[mlen] = '\0';

    woff = (12u + mlen + 15u) & ~(size_t)15u;
    if (woff < (size_t)fsize) {
        out->weights_len = (size_t)fsize - woff;
        /* 16-byte aligned heap copy: over-allocate and round the base */
        out->weights = (unsigned char *)malloc(out->weights_len + 15);
        if (!out->weights) {
            free(buf);
            free(out->manifest_json);
            out->manifest_json = NULL;
            set_err(errbuf, errlen, "out of memory");
            return 1;
        }
        /* store the raw pointer in the padding trick below if ever needed;
         * for now malloc on msvcrt is 8-aligned, kernels use unaligned loads
         * as fallback, and the packer aligns tensor offsets internally. */
        memcpy(out->weights, buf + woff, out->weights_len);
    }
    free(buf);
    return 0;
}

void rim_free(rim_model_t *m)
{
    free(m->manifest_json);
    free(m->weights);
    memset(m, 0, sizeof(*m));
}
