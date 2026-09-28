/*
 * grledger.h - GAMESYNC and GAMERES must not undo each other.
 *
 * THE DEFECT (agent 1.90.0, .110 = XP, and .243 = Win98 SE, 2026-09-28).
 * gs_run() copies a title from the library, then GAMERES (agent/src/gameres.c)
 * writes THIS box's resolution into the title's own config. That is the whole
 * point of GAMERES - one staged tree, eight monitors - but it leaves the file
 * different from the library's copy, and GAMESYNC's resume test (size AND
 * last-write time, gsresume.h) then sees a file that is not the library's and
 * copies the library's back at the next sync. GAMERES changes it again. Every
 * sync, forever:
 *
 *   Thief2\cam.cfg      library 1379 B "game_screen_size 800 600"
 *                       GAMERES -> 1380 B "game_screen_size 1024 768"
 *                       next sync: size differs -> re-copied -> GAMERES again
 *   Daggerfall\dosbox_daggerfall.conf   fullresolution desktop <-> original
 *   Descent1\DESCENT.CFG (Win98 .243)   ResolutionX/Y appended, every run
 *
 * Five syncs in a row on .110 with nothing else changing wrote 22, 20, 11, 11
 * and 20 files, GAMERES reported 23 values changed every time, and every sync
 * rebuilt the icon layout ("desktop changed ... arranging icons") - exactly
 * what the "a steady-state box must report 0 file(s) written" and "a settled
 * box must report 0 value(s) changed" signals exist to catch. Both signals
 * were dead on every box with a title GAMERES adjusts.
 *
 * THE FIX: A SMALL PER-BOX LEDGER OF WHAT GAMERES REWROTE. For each file
 * GAMERES changes it records the file as it was BEFORE the write (the base -
 * the library's copy as GAMESYNC stamped it: size and last-write time) and
 * AFTER it (the out). The resume test then skips a destination when
 *
 *     the destination is still exactly what GAMERES left  (size + time = out)
 *  && the library's file is still the one GAMERES adjusted (size + time = base)
 *
 * and copies it - after which GAMERES adjusts the new copy - when either side
 * really changed: the library shipped a new version, or someone (a person, the
 * game itself) edited the file on the box.
 *
 * THE LEDGER CAN ONLY ADD A SKIP. It is consulted only when the resume test has
 * already decided to copy, and anything it cannot vouch for leaves that
 * decision alone. So a missing, truncated, corrupt or foreign ledger is exactly
 * the old behaviour - one more copy, never one fewer. Every record carries its
 * own checksum; a record that fails it is dropped, not guessed at.
 *
 * BOUNDED: one record per destination path, at most GRL_MAX_ENTRIES (the
 * library has ~40 files GAMERES touches), oldest evicted first, and a file
 * bigger than GRL_FILE_MAX is not read at all.
 *
 * Win32-free (times are 100 ns FILETIME ticks as 64-bit integers, formatted by
 * hand because Win98's msvcrt has no %lld) so tests/native/test_grledger.c
 * compiles the code the agent runs.
 */
#ifndef RETRO_GRLEDGER_H
#define RETRO_GRLEDGER_H

#include <stddef.h>
#include <string.h>

#include "gsresume.h"       /* gsr_same_time: the SAME 2 s slack as the resume test */

#ifdef __GNUC__
#define GRL_FN static __attribute__((unused))
#else
#define GRL_FN static
#endif

#define GRL_MAX_ENTRIES 128
#define GRL_PATH_MAX    260                 /* MAX_PATH                         */
#define GRL_FILE_MAX    (64 * 1024)         /* ~45 KB when full; more = not ours */
#define GRL_HEADER      "RETRO-GRLEDGER 1"
#define GRL_DIGITS_MAX  18                  /* < 2^63; a 2026 FILETIME is 18 digits */

typedef struct {
    char      path[GRL_PATH_MAX];   /* the destination, e.g. C:\Games\Thief2\cam.cfg */
    long long base_size, base_time; /* the library's copy, as it landed on the box    */
    long long out_size,  out_time;  /* the same file after GAMERES rewrote it         */
} grl_entry_t;

typedef struct {
    grl_entry_t e[GRL_MAX_ENTRIES];
    int n;
    int dirty;                      /* changed since it was loaded / last saved      */
} grl_t;

/* grl_decide / grl_decide_source */
enum {
    GRL_SKIP = 1,       /* GAMERES's copy of an unchanged library file: keep it    */
    GRL_ASK_SOURCE,     /* sizes agree, the listing's time does not: ask the source */
    GRL_DST_CHANGED,    /* not what GAMERES left - edited on the box: no opinion    */
    GRL_SRC_CHANGED     /* the library's file changed since: take it again          */
};

/* ---------------------------------------------------------------------- */
/* helpers                                                                 */
/* ---------------------------------------------------------------------- */

GRL_FN int grl_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Windows paths compare case-insensitively (CLAUDE.md: search Windows trees
 * case-insensitively). ASCII folding is enough for C:\Games\<Title>\... */
GRL_FN int grl_path_eq(const char *a, const char *b)
{
    while (*a && *b) {
        int x = grl_lower((unsigned char)*a), y = grl_lower((unsigned char)*b);
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* FNV-1a, 32 bits: a record that does not hash to its own checksum is dropped. */
GRL_FN unsigned long grl_fnv(const char *s, size_t n)
{
    unsigned long h = 2166136261UL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h = (h * 16777619UL) & 0xFFFFFFFFUL;
    }
    return h;
}

GRL_FN size_t grl_put_num(char *out, long long v)
{
    char tmp[24];
    size_t n = 0, i;
    unsigned long long u = v < 0 ? 0 : (unsigned long long)v;
    do {
        tmp[n++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u && n < sizeof(tmp));
    for (i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* A non-negative decimal of 1..GRL_DIGITS_MAX digits followed by one space. */
GRL_FN int grl_get_num(const char **pp, const char *end, long long *out)
{
    const char *p = *pp;
    long long v = 0;
    int digits = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        if (++digits > GRL_DIGITS_MAX)
            return -1;
        v = v * 10 + (*p - '0');
        p++;
    }
    if (!digits || p >= end || *p != ' ')
        return -1;
    *out = v;
    *pp = p + 1;
    return 0;
}

GRL_FN int grl_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---------------------------------------------------------------------- */
/* the table                                                               */
/* ---------------------------------------------------------------------- */

GRL_FN void grl_reset(grl_t *l)
{
    l->n = 0;
    l->dirty = 0;
}

GRL_FN int grl_find(const grl_t *l, const char *path)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (grl_path_eq(l->e[i].path, path))
            return i;
    return -1;
}

GRL_FN void grl_remove(grl_t *l, int i)
{
    if (i < 0 || i >= l->n)
        return;
    if (i < l->n - 1)
        memmove(&l->e[i], &l->e[i + 1], (size_t)(l->n - 1 - i) * sizeof(l->e[0]));
    l->n--;
    l->dirty = 1;
}

/* Append (or replace) one record. The newest record goes last, so when the
 * table is full the OLDEST is the one evicted. Returns 1 if stored. */
GRL_FN int grl_put(grl_t *l, const char *path, long long bs, long long bt,
                   long long os, long long ot)
{
    size_t n = strlen(path);
    grl_entry_t *e;
    if (!n || n >= GRL_PATH_MAX)
        return 0;
    grl_remove(l, grl_find(l, path));
    if (l->n >= GRL_MAX_ENTRIES)
        grl_remove(l, 0);
    e = &l->e[l->n++];
    memcpy(e->path, path, n + 1);
    e->base_size = bs;
    e->base_time = bt;
    e->out_size  = os;
    e->out_time  = ot;
    l->dirty = 1;
    return 1;
}

/*
 * GAMERES changed `path` from (pre) to (post). Record it.
 *
 * CHAINING. A title often has several rules on ONE file (Descent's ResolutionX
 * and ResolutionY, Unreal's three INI keys), and the second rule's "before" is
 * the first rule's "after". So when the file was, just before this write,
 * exactly what the ledger says GAMERES last left, the base carries over: the
 * base must stay the LIBRARY's copy, not GAMERES's own intermediate output.
 * The same carries a base across syncs when the monitor changes and GAMERES
 * adjusts a file it had already adjusted.
 */
GRL_FN int grl_note(grl_t *l, const char *path, long long pre_size, long long pre_time,
                    long long post_size, long long post_time)
{
    int i = grl_find(l, path);
    long long bs = pre_size, bt = pre_time;
    if (i >= 0 && l->e[i].out_size == pre_size
        && gsr_same_time(l->e[i].out_time, pre_time)) {
        bs = l->e[i].base_size;
        bt = l->e[i].base_time;
    }
    return grl_put(l, path, bs, bt, post_size, post_time);
}

/* GAMESYNC copied the library's file over `path`: whatever GAMERES recorded
 * about the old one no longer describes anything. */
GRL_FN void grl_forget(grl_t *l, const char *path)
{
    grl_remove(l, grl_find(l, path));
}

/* ---------------------------------------------------------------------- */
/* the decision                                                            */
/* ---------------------------------------------------------------------- */

/*
 * The resume test (gsresume.h) has said COPY or ASK_SOURCE for this
 * destination, and the ledger holds record `e` for it. May the copy be
 * skipped? Stage 1 uses the source's time from the directory listing;
 * GRL_ASK_SOURCE means the caller must ask the source directly
 * (grl_decide_source), exactly as the resume test does.
 *
 * GRL_DST_CHANGED is "no opinion": the resume test's own verdict stands.
 */
GRL_FN int grl_decide(const grl_entry_t *e, long long dst_size, long long dst_time,
                      long long src_size, int have_src_list_time,
                      long long src_list_time)
{
    if (!e)
        return GRL_DST_CHANGED;
    if (dst_size != e->out_size || !gsr_same_time(dst_time, e->out_time))
        return GRL_DST_CHANGED;
    if (src_size != e->base_size)
        return GRL_SRC_CHANGED;
    if (have_src_list_time && gsr_same_time(src_list_time, e->base_time))
        return GRL_SKIP;
    return GRL_ASK_SOURCE;
}

GRL_FN int grl_decide_source(const grl_entry_t *e, int have_src_time, long long src_time)
{
    return (e && have_src_time && gsr_same_time(src_time, e->base_time))
         ? GRL_SKIP : GRL_SRC_CHANGED;
}

/* ---------------------------------------------------------------------- */
/* the file                                                                */
/* ---------------------------------------------------------------------- */

/*
 * One record per line:
 *
 *   <fnv1a hex8> <base_size> <base_time> <out_size> <out_time> <path>\r\n
 *
 * The checksum covers everything after it and its space, up to the end of the
 * line. Returns the length written, or -1 if `cap` is too small (the caller
 * then writes nothing - a missing ledger is the old behaviour).
 */
GRL_FN int grl_format(const grl_t *l, char *out, size_t cap)
{
    size_t at = 0, hl = strlen(GRL_HEADER);
    int i;
    static const char hex[] = "0123456789abcdef";

    if (cap < hl + 3)
        return -1;
    memcpy(out, GRL_HEADER "\r\n", hl + 2);
    at = hl + 2;
    for (i = 0; i < l->n; i++) {
        const grl_entry_t *e = &l->e[i];
        char body[4 * (GRL_DIGITS_MAX + 2) + GRL_PATH_MAX + 4];
        size_t b = 0, pl = strlen(e->path);
        unsigned long h;
        int k;
        if (!pl || pl >= GRL_PATH_MAX)
            continue;
        b += grl_put_num(body + b, e->base_size); body[b++] = ' ';
        b += grl_put_num(body + b, e->base_time); body[b++] = ' ';
        b += grl_put_num(body + b, e->out_size);  body[b++] = ' ';
        b += grl_put_num(body + b, e->out_time);  body[b++] = ' ';
        memcpy(body + b, e->path, pl);
        b += pl;
        if (at + 9 + b + 2 + 1 > cap)
            return -1;
        h = grl_fnv(body, b);
        for (k = 7; k >= 0; k--)
            out[at++] = hex[(h >> (k * 4)) & 0xF];
        out[at++] = ' ';
        memcpy(out + at, body, b);
        at += b;
        out[at++] = '\r';
        out[at++] = '\n';
    }
    out[at] = 0;
    return (int)at;
}

/* Parse ONE line (without its CR/LF) into the table. 0 = stored, -1 = bad. */
GRL_FN int grl_parse_line(grl_t *l, const char *p, const char *end)
{
    unsigned long want = 0, got;
    long long bs, bt, os, ot;
    const char *body;
    char path[GRL_PATH_MAX];
    size_t pl;
    int k;

    if (end - p < 9 + 9)             /* "xxxxxxxx " + "0 0 0 0 x" */
        return -1;
    for (k = 0; k < 8; k++) {
        int v = grl_hexval((unsigned char)p[k]);
        if (v < 0)
            return -1;
        want = (want << 4) | (unsigned long)v;
    }
    if (p[8] != ' ')
        return -1;
    body = p + 9;
    got = grl_fnv(body, (size_t)(end - body));
    if (got != want)
        return -1;
    p = body;
    if (grl_get_num(&p, end, &bs) || grl_get_num(&p, end, &bt)
        || grl_get_num(&p, end, &os) || grl_get_num(&p, end, &ot))
        return -1;
    pl = (size_t)(end - p);
    if (!pl || pl >= GRL_PATH_MAX)
        return -1;
    for (k = 0; k < (int)pl; k++)
        if ((unsigned char)p[k] < 0x20)
            return -1;
    memcpy(path, p, pl);
    path[pl] = 0;
    return grl_put(l, path, bs, bt, os, ot) ? 0 : -1;
}

/*
 * Load a ledger file's text into `l` (which is reset first). Returns the
 * number of records kept, or -1 if the text is not a ledger at all (wrong or
 * missing header, too big) - either way `l` then holds only what can be
 * trusted. *bad receives the number of lines dropped.
 */
GRL_FN int grl_parse(grl_t *l, const char *text, size_t len, int *bad)
{
    const char *p = text, *end = text + len;
    size_t hl = strlen(GRL_HEADER);
    int dropped = 0;

    grl_reset(l);
    if (bad)
        *bad = 0;
    if (!text || len > GRL_FILE_MAX || len < hl
        || memcmp(text, GRL_HEADER, hl) != 0
        || (len > hl && text[hl] != '\r' && text[hl] != '\n'))
        return -1;
    p = text + hl;
    while (p < end) {
        const char *ls, *le;
        while (p < end && (*p == '\r' || *p == '\n'))
            p++;
        if (p >= end)
            break;
        ls = p;
        while (p < end && *p != '\r' && *p != '\n')
            p++;
        le = p;
        if (grl_parse_line(l, ls, le) < 0)
            dropped++;
    }
    if (bad)
        *bad = dropped;
    /* A file with dropped lines is rewritten clean at the next save. */
    l->dirty = dropped > 0;
    return l->n;
}

#endif /* RETRO_GRLEDGER_H */
