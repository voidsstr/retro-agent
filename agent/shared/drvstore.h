/*
 * drvstore.h - which index file of a driver store holds an id (agent 1.89.0).
 *
 * The store (the image's driver tree on the share) is indexed by
 * scripts/fleet/drvindex.c with the agent's own matcher; the lines are split
 * into DRVINDEX\<bucket>.TXT so a box fetches only what its own devices need,
 * not 47,000 lines over SMB. BOTH sides compute the bucket with THIS function
 * - the indexer prints it, the agent looks it up - so they cannot disagree.
 *
 *   PCI\VEN_10DE&...       -> PCI_10DE
 *   USB\VID_0451&PID_...   -> USB_0451
 *   HDAUDIO\FUNC_01&VEN_10EC&... -> HDA_10EC
 *   anything else          -> OTHER
 */
#ifndef RETRO_DRVSTORE_H
#define RETRO_DRVSTORE_H

#include <string.h>

#if defined(__GNUC__)
#define DRVSTORE_API static __attribute__((unused))
#else
#define DRVSTORE_API static
#endif

DRVSTORE_API int drvstore_up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

DRVSTORE_API int drvstore_hex4(const char *p, char *out)
{
    int i;
    for (i = 0; i < 4; i++) {
        int c = drvstore_up((unsigned char)p[i]);
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return 0;
        out[i] = (char)c;
    }
    out[4] = 0;
    return 1;
}

/* Case-insensitive: does `s` start with `pre`? */
DRVSTORE_API int drvstore_starts(const char *s, const char *pre)
{
    for (; *pre; s++, pre++)
        if (!*s || drvstore_up((unsigned char)*s) != drvstore_up((unsigned char)*pre)) return 0;
    return 1;
}

/* Find "<key>" (case-insensitive) in s and return what follows it, or NULL. */
DRVSTORE_API const char *drvstore_after(const char *s, const char *key)
{
    size_t n = strlen(key);
    for (; *s; s++)
        if (drvstore_starts(s, key)) return s + n;
    return NULL;
}

/* The bucket name for one id, into out (>= 9 bytes). */
DRVSTORE_API void drvstore_bucket(const char *id, char *out)
{
    const char *v;
    char h[5];
    if (drvstore_starts(id, "PCI\\") && (v = drvstore_after(id, "VEN_")) && drvstore_hex4(v, h)) {
        memcpy(out, "PCI_", 4); memcpy(out + 4, h, 5); return;
    }
    if (drvstore_starts(id, "USB\\") && (v = drvstore_after(id, "VID_")) && drvstore_hex4(v, h)) {
        memcpy(out, "USB_", 4); memcpy(out + 4, h, 5); return;
    }
    if (drvstore_starts(id, "HDAUDIO\\") && (v = drvstore_after(id, "VEN_")) && drvstore_hex4(v, h)) {
        memcpy(out, "HDA_", 4); memcpy(out + 4, h, 5); return;
    }
    strcpy(out, "OTHER");
}

/* The next line of an index bucket whose id (first field) equals `id`,
 * case-insensitively and WHOLE - "PCI\VEN_10DE&DEV_0150" must not match a line
 * for "...DEV_0150&SUBSYS_...". Its "DIR\INF" (second field) goes to rel.
 * Advances *cursor past the line; returns 0 at the end of the text. */
DRVSTORE_API int drvstore_match(const char **cursor, const char *id, char *rel, size_t cap)
{
    const char *line = *cursor, *next;
    size_t      idl = strlen(id);

    for (; line && *line; line = next) {
        const char *f;
        size_t      k = 0, i;
        int         eq = 1;
        next = strchr(line, '\n');
        next = next ? next + 1 : line + strlen(line);
        for (i = 0; i < idl; i++)
            if (line + i >= next || drvstore_up((unsigned char)line[i]) != drvstore_up((unsigned char)id[i])) {
                eq = 0;
                break;
            }
        if (!eq || !idl || line[idl] != '\t')
            continue;
        for (f = line + idl + 1; f < next && *f != '\t' && *f != '\r' && *f != '\n' && k + 1 < cap; f++)
            rel[k++] = *f;
        rel[k] = 0;
        if (!k)
            continue;
        *cursor = next;
        return 1;
    }
    *cursor = line;
    return 0;
}

/* Is an index path safe to join onto the store root and copy from? Exactly
 * "DIR\FILE.INF": one backslash, a short directory, no "..", no drive or UNC,
 * nothing a poisoned index could use to reach outside the store. */
DRVSTORE_API int drvstore_rel_ok(const char *rel)
{
    const char *bs, *p;
    size_t      n;
    if (!rel || !*rel || rel[0] == '\\' || strstr(rel, "..") || strchr(rel, ':') || strchr(rel, '/'))
        return 0;
    if (!(bs = strchr(rel, '\\')) || strchr(bs + 1, '\\'))
        return 0;
    if ((size_t)(bs - rel) >= 64 || !bs[1])
        return 0;
    n = strlen(bs + 1);
    if (n < 5)
        return 0;
    p = bs + 1 + n - 4;
    return p[0] == '.' && drvstore_up((unsigned char)p[1]) == 'I' && drvstore_up((unsigned char)p[2]) == 'N'
        && drvstore_up((unsigned char)p[3]) == 'F';
}

#endif
