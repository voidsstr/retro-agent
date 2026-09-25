/*
 * gimatch.h - the pure logic behind GAMEINDEX's disk scan (agent 1.85.0).
 *
 * WHAT A PASS USED TO COST (agent <= 1.84.x, measured 6-10 s on average and up
 * to 65 s per pass): every 240 s, forever,
 *   - every directory to depth 3 under every game root was tested against the
 *     whole signature table with one GetFileAttributesA PER SIGNATURE - ~68
 *     path lookups per directory, each a linear directory search on FAT - on
 *     top of the FindFirstFile enumeration the walk already did to recurse;
 *   - C:\Games (and Program Files\Games) was walked twice, once from the drive
 *     root and once as a root of its own;
 *   - every desktop and Start-menu shortcut was resolved through COM;
 *   - and all of it ran whether or not anything on the disk had changed.
 *
 * NOW:
 *   - ONE enumeration per directory. Each entry the walk already reads is fed
 *     to gim_note(), which compares its long AND 8.3 name against the table in
 *     memory - the same two names GetFileAttributesA would have matched - and
 *     gim_matches() then answers exactly what the probes answered: "exe is a
 *     FILE here, and moddir (if any) is a DIRECTORY here".
 *   - a cheap fingerprint of the places a game install shows up (the root
 *     listings, the game roots' entries, the shortcut folders, the uninstall
 *     key) decides whether a pass is needed at all - gi_scan_reason() - with a
 *     forced full pass as a safety net for changes the fingerprint cannot see.
 *   - the last index is kept on disk (gi_cache_*), so a reboot serves the
 *     previous run's answer at once instead of forcing a scan.
 *
 * Header-only and free of Win32, so tests/native/test_gimatch.c compiles the
 * code the agent runs.
 */
#ifndef RETRO_GIMATCH_H
#define RETRO_GIMATCH_H

#include <string.h>

#ifdef __GNUC__
#define GIM_UNUSED __attribute__((unused))
#else
#define GIM_UNUSED
#endif

/*
 * `engine` tells the host which server-query protocol and which favourites
 * mechanism apply. "-" means we can detect the game but have no server browser
 * to populate, which is still worth reporting.
 *
 * `moddir`, when set, must exist as a subdirectory next to the exe. That is how
 * the GoldSrc family is split apart: Half-Life, Counter-Strike and The
 * Specialists are all hl.exe, distinguished only by the mod directory.
 */
typedef struct {
    const char *key;
    const char *name;
    const char *exe;
    const char *moddir;
    const char *engine;
} game_sig_t;

#define GIM_MAX_SIGS 128

/* The signature table with the lengths precomputed, so the per-entry test can
 * reject on length before comparing a single character. */
typedef struct {
    const game_sig_t *sigs;
    int               n;
    unsigned short    exe_len[GIM_MAX_SIGS];
    unsigned short    mod_len[GIM_MAX_SIGS];
} gim_table_t;

/* What one directory listing contained, per signature. */
typedef struct {
    unsigned char exe[GIM_MAX_SIGS];   /* sig's exe is a FILE in this dir      */
    unsigned char mod[GIM_MAX_SIGS];   /* sig's moddir is a DIRECTORY in it    */
} gim_hits_t;

/* Returns the number of signatures, or -1 if the table outgrew GIM_MAX_SIGS
 * (the caller must refuse loudly rather than silently drop the tail). */
GIM_UNUSED static int gim_table_init(gim_table_t *t, const game_sig_t *sigs)
{
    int i;
    t->sigs = sigs;
    for (i = 0; sigs[i].key; i++) {
        if (i >= GIM_MAX_SIGS) {
            t->n = GIM_MAX_SIGS;
            return -1;
        }
        t->exe_len[i] = (unsigned short)strlen(sigs[i].exe);
        t->mod_len[i] = (unsigned short)(sigs[i].moddir ? strlen(sigs[i].moddir) : 0);
    }
    t->n = i;
    return i;
}

GIM_UNUSED static void gim_reset(gim_hits_t *h)
{
    memset(h, 0, sizeof(*h));
}

/* ASCII case-insensitive equality of a[0..n) and b[0..n). Filesystem name
 * matching folds ASCII case only, and every signature is ASCII, so this is
 * exactly the comparison the old GetFileAttributesA probe made. */
GIM_UNUSED static int gim_ieq_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y - 'A' + 'a');
        if (x != y)
            return 0;
    }
    return 1;
}

/* Feed one directory entry. `alt` is its 8.3 name ("" when the long name is
 * already 8.3) - a path probe matched either, so this must too. */
GIM_UNUSED static void gim_note(gim_hits_t *h, const gim_table_t *t,
                                const char *name, const char *alt, int is_dir)
{
    size_t nl = strlen(name), al = alt ? strlen(alt) : 0;
    int i;

    if ((nl == 1 && name[0] == '.') ||
        (nl == 2 && name[0] == '.' && name[1] == '.'))
        return;
    for (i = 0; i < t->n; i++) {
        if (is_dir) {
            size_t ml = t->mod_len[i];
            if (!ml)
                continue;
            if ((nl == ml && gim_ieq_n(name, t->sigs[i].moddir, ml)) ||
                (al == ml && gim_ieq_n(alt, t->sigs[i].moddir, ml)))
                h->mod[i] = 1;
        } else {
            size_t el = t->exe_len[i];
            if ((nl == el && gim_ieq_n(name, t->sigs[i].exe, el)) ||
                (al == el && gim_ieq_n(alt, t->sigs[i].exe, el)))
                h->exe[i] = 1;
        }
    }
}

/* Does signature i match the directory whose entries were fed in? */
GIM_UNUSED static int gim_matches(const gim_hits_t *h, const gim_table_t *t, int i)
{
    if (!h->exe[i])
        return 0;
    return t->mod_len[i] == 0 || h->mod[i];
}

/* ------------------------------------------------------------------ */
/* When is a pass needed?                                              */
/* ------------------------------------------------------------------ */

enum {
    GI_SCAN_SKIP = 0,     /* fingerprint unchanged, safety net not due       */
    GI_SCAN_FIRST,        /* no index at all yet                             */
    GI_SCAN_POKED,        /* GAMESYNC just deployed something                */
    GI_SCAN_CHANGED,      /* the fingerprint moved                           */
    GI_SCAN_FULL_DUE      /* the forced full pass is due                     */
};

GIM_UNUSED static int gi_scan_reason(int have_index,
                                     unsigned long fp_now,
                                     unsigned long fp_index,
                                     unsigned long ms_since_full,
                                     unsigned long full_every_ms,
                                     int poked)
{
    if (!have_index)
        return GI_SCAN_FIRST;
    if (poked)
        return GI_SCAN_POKED;
    if (fp_now != fp_index)
        return GI_SCAN_CHANGED;
    if (ms_since_full >= full_every_ms)
        return GI_SCAN_FULL_DUE;
    return GI_SCAN_SKIP;
}

GIM_UNUSED static const char *gi_scan_reason_name(int r)
{
    switch (r) {
    case GI_SCAN_FIRST:    return "first";
    case GI_SCAN_POKED:    return "after GAMESYNC";
    case GI_SCAN_CHANGED:  return "changed";
    case GI_SCAN_FULL_DUE: return "hourly";
    default:               return "skip";
    }
}

/* The fingerprint is a SUM of per-entry hashes, so it does not depend on the
 * order a filesystem enumerates in (NTFS sorts, FAT does not). */
GIM_UNUSED static unsigned long gi_fnv1a(const char *s)
{
    unsigned long h = 2166136261UL;
    while (*s) {
        h ^= (unsigned long)(unsigned char)(*s++);
        h *= 16777619UL;
        h &= 0xFFFFFFFFUL;
    }
    return h;
}

/* One entry's contribution: its name (ASCII case folded - a rename that only
 * changes case is not an install), whether it is a directory, and - when the
 * caller wants it - its last-write time. */
GIM_UNUSED static unsigned long gi_fp_entry(const char *name, int is_dir,
                                            unsigned long time_lo,
                                            unsigned long time_hi)
{
    char low[272];
    size_t i;
    unsigned long h;
    for (i = 0; name[i] && i < sizeof(low) - 1; i++) {
        char c = name[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    low[i] = 0;
    h = gi_fnv1a(low);
    h ^= is_dir ? 0x9E3779B9UL : 0;
    h += (time_lo * 2654435761UL) ^ time_hi;
    return h & 0xFFFFFFFFUL;
}

/* ------------------------------------------------------------------ */
/* The on-disk cache: "GICACHE1 <fp> <hash> <len>\n<json>"             */
/* ------------------------------------------------------------------ */

#define GI_CACHE_MAGIC     "GICACHE1 "
#define GI_CACHE_HDR_MAX   40

GIM_UNUSED static void gi_hex8(char *out, unsigned long v)
{
    static const char d[] = "0123456789abcdef";
    int i;
    for (i = 7; i >= 0; i--) {
        out[i] = d[v & 0xF];
        v >>= 4;
    }
}

/* Writes the header (with its trailing newline) into out[GI_CACHE_HDR_MAX];
 * returns its length. */
GIM_UNUSED static int gi_cache_header(char *out, unsigned long fp,
                                      unsigned long hash, unsigned long len)
{
    char dec[12];
    int n = 0, k = 0;
    memcpy(out, GI_CACHE_MAGIC, 9);
    n = 9;
    gi_hex8(out + n, fp);   n += 8;
    out[n++] = ' ';
    gi_hex8(out + n, hash); n += 8;
    out[n++] = ' ';
    do {
        dec[k++] = (char)('0' + len % 10);
        len /= 10;
    } while (len && k < (int)sizeof(dec));
    while (k)
        out[n++] = dec[--k];
    out[n++] = '\n';
    out[n] = 0;
    return n;
}

GIM_UNUSED static int gi_hexval(const char *s, unsigned long *v)
{
    int i;
    unsigned long r = 0;
    for (i = 0; i < 8; i++) {
        char c = s[i];
        r <<= 4;
        if (c >= '0' && c <= '9')      r |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') r |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') r |= (unsigned long)(c - 'A' + 10);
        else return 0;
    }
    *v = r;
    return 1;
}

/* Validate a whole cache file. On success sets fp/hash and the offset and
 * length of the JSON document and returns 1. A torn or foreign file returns 0
 * and is simply ignored - the next pass rebuilds it. */
GIM_UNUSED static int gi_cache_parse(const char *buf, unsigned long buflen,
                                     unsigned long *fp, unsigned long *hash,
                                     unsigned long *off, unsigned long *len)
{
    unsigned long i, l = 0;
    if (buflen < 9 + 8 + 1 + 8 + 1 + 1 + 1 + 2 || memcmp(buf, GI_CACHE_MAGIC, 9))
        return 0;
    if (!gi_hexval(buf + 9, fp) || buf[17] != ' ' ||
        !gi_hexval(buf + 18, hash) || buf[26] != ' ')
        return 0;
    for (i = 27; i < buflen && i < 27 + 10 && buf[i] >= '0' && buf[i] <= '9'; i++)
        l = l * 10 + (unsigned long)(buf[i] - '0');
    if (i == 27 || i >= buflen || buf[i] != '\n')
        return 0;
    i++;
    if (l < 2 || i + l != buflen || buf[i] != '{' || buf[i + l - 1] != '}')
        return 0;
    *off = i;
    *len = l;
    return 1;
}

#endif /* RETRO_GIMATCH_H */
