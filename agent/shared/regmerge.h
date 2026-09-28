/*
 * regmerge.h - how GAMESYNC merges a title's install.reg, and how it PROVES it.
 *
 * THE DEFECT (agent 1.86.1, .243 = Windows 98 SE on a Pentium P54C,
 * 2026-09-28). gs_merge_reg() in agent/src/gamesync.c started regedit with
 *
 *     CreateProcessA(NULL, "regedit /s \"C:\\Games\\HexenII\\install.reg\"",
 *                    NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)
 *
 * from the library-sync thread, and on Windows 98 that call returned FALSE:
 *
 *     [11:24:01][GAMESYNC] HexenII: cannot run regedit (0) - game may not launch
 *
 * So a fresh Win9x box never had ANY title's install.reg merged - and Hexen
 * II's is not optional (without its RAID value the engine quits with "You
 * need to re-install Hexen 2"). The identical call merges on every XP box.
 *
 * What the Windows 98 box DOES start, measured in its own agent.log - every
 * one of them unlike the failing call in each of these ways:
 *
 *   - EXEC `command.com /c regedit /s C:\WINDOWS\TEMP\RUNKEY.REG` (and
 *     `regedit /e ...`): exit 0, the .reg applied - an 8.3 path, UNQUOTED,
 *     lpCurrentDirectory "C:\", from the command thread;
 *   - REBOOT (1.85.4): `rundll32.exe shell32.dll,SHExitWindowsEx 6` started
 *     CREATE_SUSPENDED and then resumed - hardware-proven.
 *
 * The failing call was the only one that (a) left the module to the search
 * order ("regedit", no path, no extension), (b) inherited whatever current
 * directory the agent happened to have, (c) ran from a THREAD_PRIORITY_IDLE
 * background thread, and (d) used creation flags 0 - which on Windows 9x
 * means CreateProcess does not return until the child has finished its own
 * initialisation (its DLLs loaded and attached), and a child that fails there
 * is reported to the PARENT as CreateProcess failing. Error 0 is Windows 98
 * failing a call without saying why; which of (a)-(d) it was is not proven
 * from here, so the 9x path now removes all four at once:
 *
 *   - the ABSOLUTE path of %windir%\REGEDIT.EXE, checked to exist first;
 *   - the .reg as its 8.3 short path, unquoted (the form proven by EXEC);
 *     quoted only if it still contains a space;
 *   - an explicit lpCurrentDirectory (the Windows directory);
 *   - CREATE_SUSPENDED, then ResumeThread - never CREATE_NO_WINDOW or
 *     CREATE_UNICODE_ENVIRONMENT, which 9x rejects with error 87;
 *   - the caller raises the thread to normal priority for the few seconds
 *     the merge takes, and waits pumping messages (the sync thread is a COM
 *     apartment, so a broadcast from the child would otherwise hang on it).
 *
 * NT keeps its command line and flags exactly as before - that form is
 * proven on every XP box and there is no reason to touch it.
 *
 * AND A LAUNCH IS NOT A MERGE. "regedit started and exited 0" is the return
 * value, not the post-condition - Win98's regedit /s exits 0 whatever it did.
 * So this header also READS the REGEDIT4 file, and the agent checks every
 * value it names against the live registry afterwards. Where regedit did not
 * land them (it could not start, it failed, or on Win9x any value is simply
 * not there), the agent writes those values itself and checks again - and
 * says loudly that regedit did not do it. A tolerated failure must say it is
 * a failure (CLAUDE.md).
 *
 * The reader is deliberately small and exact: the syntax the staged library
 * uses (audited 2026-09-28: 31 files, all REGEDIT4 - strings with only \\ and
 * \" escapes, dword:, hex: with continuations, hex(N):, "name"=-). Anything
 * else is RM_OP_UNKNOWN - neither verified nor written - because a reader
 * that guesses would then "repair" a correct value into a wrong one.
 *
 * Win32-free so tests/native/test_regmerge.c compiles the code the agent runs.
 */
#ifndef RETRO_REGMERGE_H
#define RETRO_REGMERGE_H

#include <stddef.h>
#include <string.h>

#ifdef __GNUC__
#define RM_UNUSED __attribute__((unused))
#else
#define RM_UNUSED
#endif

/* winbase.h CreateProcess flags, spelled out so this stays Win32-free. */
#define RM_CREATE_SUSPENDED           0x00000004u
#define RM_CREATE_UNICODE_ENVIRONMENT 0x00000400u
#define RM_CREATE_NO_WINDOW           0x08000000u

/* winnt.h value types */
#define RM_REG_SZ         1u
#define RM_REG_EXPAND_SZ  2u
#define RM_REG_BINARY     3u
#define RM_REG_DWORD      4u
#define RM_REG_MULTI_SZ   7u

/* registry roots a REGEDIT4 file can name */
enum {
    RM_ROOT_NONE = 0,
    RM_HKCR, RM_HKCU, RM_HKLM, RM_HKU, RM_HKCC, RM_HKDD
};

/* what one entry asks for */
enum {
    RM_OP_SET = 1,      /* "name"=<data>                     */
    RM_OP_DELVALUE,     /* "name"=-                          */
    RM_OP_DELKEY,       /* [-HKEY_...\key]                   */
    RM_OP_UNKNOWN       /* a line this reader will not guess */
};

/* the file's header */
enum {
    RM_DIALECT_NONE = 0,    /* no header line: regedit would refuse it */
    RM_DIALECT_REGEDIT4,    /* merges on 9x and NT                     */
    RM_DIALECT_V5           /* "Windows Registry Editor Version 5.00"  */
};

#define RM_KEY_MAX   512
#define RM_NAME_MAX  256
#define RM_DATA_MAX  2048
#define RM_LINE_MAX  8192

typedef struct {
    int      op;                    /* RM_OP_*                                   */
    int      root;                  /* RM_HK*; RM_ROOT_NONE => op is UNKNOWN     */
    char     key[RM_KEY_MAX];       /* subkey below the root                     */
    char     name[RM_NAME_MAX];     /* value name; "" is the default value (@)   */
    unsigned type;                  /* RM_REG_* for RM_OP_SET                    */
    unsigned char data[RM_DATA_MAX];/* RM_REG_SZ: the string, NUL-terminated     */
    unsigned len;                   /* bytes of data; for RM_REG_SZ the string
                                       length WITHOUT its NUL                    */
    int      lineno;                /* 1-based physical line the entry began on  */
} rm_entry_t;

typedef struct {
    const char *p, *end;
    int  lineno;
    int  dialect;
    int  root;                      /* current [key]                             */
    char key[RM_KEY_MAX];
    int  key_ok;                    /* a usable current key                      */
    char line[RM_LINE_MAX];         /* one LOGICAL line (continuations joined)   */
} rm_parser_t;

/* ---------------------------------------------------------------------- */
/* the regedit command line                                                */
/* ---------------------------------------------------------------------- */

/* Append src to out (bounded). Returns 0, or -1 if it did not fit. */
static RM_UNUSED int rm_cat(char *out, size_t outsz, const char *src)
{
    size_t have = strlen(out), add = strlen(src);
    if (have + add + 1 > outsz)
        return -1;
    memcpy(out + have, src, add + 1);
    return 0;
}

/* Append one command-line argument, quoted only when it has to be. */
static RM_UNUSED int rm_cat_arg(char *out, size_t outsz, const char *arg)
{
    if (strchr(arg, ' ') || strchr(arg, '\t'))
        return rm_cat(out, outsz, "\"") || rm_cat(out, outsz, arg) ||
               rm_cat(out, outsz, "\"") ? -1 : 0;
    return rm_cat(out, outsz, arg);
}

/*
 * Build the command line gs_merge_reg() hands to CreateProcessA, and the
 * creation flags that go with it.
 *
 *   is_9x    1 on Windows 95/98/ME (GetVersion() high bit)
 *   regedit  the ABSOLUTE path of REGEDIT.EXE - required on 9x, unused on NT
 *   reg      the install.reg to merge; on 9x the caller passes its 8.3 form
 *
 * NT:  regedit /s "<reg>"             flags 0            (unchanged, proven)
 * 9x:  <regedit> /s <reg>             CREATE_SUSPENDED   (see the header)
 *
 * Returns 0, or -1 if an argument is missing or the result would not fit.
 */
static RM_UNUSED int rm_build_cmd(int is_9x, const char *regedit, const char *reg,
                                  char *out, size_t outsz, unsigned *flags)
{
    if (!out || outsz == 0 || !flags || !reg || !reg[0])
        return -1;
    out[0] = 0;
    if (!is_9x) {
        *flags = 0;
        return rm_cat(out, outsz, "regedit /s \"") || rm_cat(out, outsz, reg) ||
               rm_cat(out, outsz, "\"") ? -1 : 0;
    }
    if (!regedit || !regedit[0])
        return -1;
    *flags = RM_CREATE_SUSPENDED;
    return rm_cat_arg(out, outsz, regedit) || rm_cat(out, outsz, " /s ") ||
           rm_cat_arg(out, outsz, reg) ? -1 : 0;
}

/* The form 1.86.1 used on EVERY Windows - kept so the test can assert the 9x
 * path no longer produces it. Never called by the agent. */
static RM_UNUSED int rm_build_cmd_1861(const char *reg, char *out, size_t outsz,
                                       unsigned *flags)
{
    *flags = 0;
    out[0] = 0;
    return rm_cat(out, outsz, "regedit /s \"") || rm_cat(out, outsz, reg) ||
           rm_cat(out, outsz, "\"") ? -1 : 0;
}

/* ---------------------------------------------------------------------- */
/* the REGEDIT4 reader                                                     */
/* ---------------------------------------------------------------------- */

static RM_UNUSED int rm_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static RM_UNUSED int rm_prefix_ci(const char *s, const char *pfx)
{
    while (*pfx) {
        if (rm_lower((unsigned char)*s) != rm_lower((unsigned char)*pfx))
            return 0;
        s++;
        pfx++;
    }
    return 1;
}

static RM_UNUSED int rm_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = rm_lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Map a root name to RM_HK*. Accepts the long names regedit writes and the
 * short forms. Returns the length consumed via *used. */
static RM_UNUSED int rm_root(const char *s, size_t *used)
{
    static const struct { const char *name; int root; } roots[] = {
        { "HKEY_LOCAL_MACHINE",  RM_HKLM }, { "HKLM", RM_HKLM },
        { "HKEY_CURRENT_USER",   RM_HKCU }, { "HKCU", RM_HKCU },
        { "HKEY_CLASSES_ROOT",   RM_HKCR }, { "HKCR", RM_HKCR },
        { "HKEY_USERS",          RM_HKU  }, { "HKU",  RM_HKU  },
        { "HKEY_CURRENT_CONFIG", RM_HKCC }, { "HKCC", RM_HKCC },
        { "HKEY_DYN_DATA",       RM_HKDD },
    };
    size_t i, n = 0;
    while (s[n] && s[n] != '\\' && s[n] != ']')
        n++;
    for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (strlen(roots[i].name) == n && rm_prefix_ci(s, roots[i].name)) {
            *used = n;
            return roots[i].root;
        }
    return RM_ROOT_NONE;
}

static RM_UNUSED void rm_init(rm_parser_t *ps, const char *text, size_t len)
{
    memset(ps, 0, sizeof(*ps));
    ps->p = text;
    ps->end = text ? text + len : text;
    /* A UTF-16 file (what Windows' own regedit exports in the v5 dialect)
     * cannot be read here. Leave dialect NONE and yield nothing. */
    if (len >= 2 && (unsigned char)text[0] == 0xFF && (unsigned char)text[1] == 0xFE)
        ps->p = ps->end;
}

/* Read one physical line into buf (without CR/LF). Returns 0 at end. */
static RM_UNUSED int rm_phys(rm_parser_t *ps, char *buf, size_t bufsz, int *overflow)
{
    size_t n = 0;
    if (ps->p >= ps->end)
        return 0;
    while (ps->p < ps->end && *ps->p != '\n') {
        if (*ps->p != '\r') {
            if (n + 1 < bufsz)
                buf[n++] = *ps->p;
            else
                *overflow = 1;
        }
        ps->p++;
    }
    if (ps->p < ps->end)
        ps->p++;                            /* the '\n' */
    buf[n] = 0;
    ps->lineno++;
    return 1;
}

static RM_UNUSED void rm_rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

/* Read one LOGICAL line: a data line ending in '\' continues on the next
 * physical line (regedit wraps hex: data that way). Comment lines never
 * continue - a "; see C:\GAMES\" comment must not swallow the next value. */
static RM_UNUSED int rm_logical(rm_parser_t *ps, int *overflow)
{
    char *line = ps->line;
    char part[RM_LINE_MAX];
    size_t n;

    if (!rm_phys(ps, line, sizeof(ps->line), overflow))
        return 0;
    rm_rtrim(line);
    if (line[0] == ';' || line[0] == '[')
        return 1;
    for (;;) {
        char *q;
        n = strlen(line);
        if (!n || line[n - 1] != '\\')
            return 1;
        line[n - 1] = 0;                    /* drop the continuation mark */
        if (!rm_phys(ps, part, sizeof(part), overflow))
            return 1;
        rm_rtrim(part);
        for (q = part; *q == ' ' || *q == '\t'; q++)
            ;
        n = strlen(line);
        if (n + strlen(q) + 1 > sizeof(ps->line)) {
            *overflow = 1;
            return 1;
        }
        memcpy(line + n, q, strlen(q) + 1);
    }
}

/* Parse a REGEDIT4 quoted string starting at *pp (on the opening quote).
 * Only \\ and \" are escapes; any other backslash sequence fails the parse
 * (returns -1) rather than guessing what the importer would make of it. */
static RM_UNUSED int rm_qstring(const char **pp, char *out, size_t outsz, size_t *outlen)
{
    const char *p = *pp;
    size_t n = 0;
    if (*p != '"')
        return -1;
    p++;
    for (;;) {
        char c = *p;
        if (!c)
            return -1;                      /* unterminated */
        if (c == '"') {
            p++;
            break;
        }
        if (c == '\\') {
            if (p[1] != '\\' && p[1] != '"')
                return -1;
            c = p[1];
            p++;
        }
        if (n + 1 >= outsz)
            return -1;
        out[n++] = c;
        p++;
    }
    out[n] = 0;
    *pp = p;
    if (outlen)
        *outlen = n;
    return 0;
}

static RM_UNUSED const char *rm_skipws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* Parse the data half of a value line into e. Returns 0, or -1 if the line
 * is not something this reader understands exactly. */
static RM_UNUSED int rm_data(const char *p, rm_entry_t *e)
{
    p = rm_skipws(p);
    if (*p == '-' && !*rm_skipws(p + 1)) {
        e->op = RM_OP_DELVALUE;
        return 0;
    }
    if (*p == '"') {
        size_t n;
        if (rm_qstring(&p, (char *)e->data, sizeof(e->data), &n) < 0)
            return -1;
        if (*rm_skipws(p))
            return -1;                      /* trailing junk after the string */
        e->type = RM_REG_SZ;
        e->len = (unsigned)n;
        e->op = RM_OP_SET;
        return 0;
    }
    if (rm_prefix_ci(p, "dword:")) {
        unsigned long v = 0;
        int digits = 0, h;
        p += 6;
        while ((h = rm_hexval((unsigned char)*p)) >= 0) {
            if (++digits > 8)
                return -1;
            v = (v << 4) | (unsigned long)h;
            p++;
        }
        if (!digits || *rm_skipws(p))
            return -1;
        e->type = RM_REG_DWORD;
        e->data[0] = (unsigned char)(v & 0xFF);
        e->data[1] = (unsigned char)((v >> 8) & 0xFF);
        e->data[2] = (unsigned char)((v >> 16) & 0xFF);
        e->data[3] = (unsigned char)((v >> 24) & 0xFF);
        e->len = 4;
        e->op = RM_OP_SET;
        return 0;
    }
    if (rm_prefix_ci(p, "hex")) {
        unsigned type = RM_REG_BINARY, n = 0;
        p += 3;
        if (*p == '(') {
            int h, digits = 0;
            type = 0;
            p++;
            while ((h = rm_hexval((unsigned char)*p)) >= 0) {
                if (++digits > 8)
                    return -1;
                type = (type << 4) | (unsigned)h;
                p++;
            }
            if (!digits || *p != ')')
                return -1;
            p++;
        }
        if (*p != ':')
            return -1;
        p++;
        for (;;) {
            int hi, lo;
            p = rm_skipws(p);
            if (!*p)
                break;
            hi = rm_hexval((unsigned char)*p);
            if (hi < 0)
                return -1;
            p++;
            lo = rm_hexval((unsigned char)*p);
            if (lo >= 0) {
                hi = (hi << 4) | lo;
                p++;
            }
            if (n >= sizeof(e->data))
                return -1;
            e->data[n++] = (unsigned char)hi;
            p = rm_skipws(p);
            if (*p == ',') {
                p++;
                continue;
            }
            if (*p)
                return -1;
        }
        e->type = type;
        e->len = n;
        e->op = RM_OP_SET;
        return 0;
    }
    return -1;
}

/*
 * Yield the next entry: a value to set or delete, a key to delete, or an
 * UNKNOWN line. Key lines that only select a key yield nothing themselves.
 * Returns 1 with *e filled, 0 at the end of the file.
 */
static RM_UNUSED int rm_next(rm_parser_t *ps, rm_entry_t *e)
{
    for (;;) {
        int overflow = 0, start = ps->lineno + 1;   /* where the entry BEGINS */
        const char *p;

        if (!rm_logical(ps, &overflow))
            return 0;
        p = rm_skipws(ps->line);

        if (!*p || *p == ';')
            continue;

        if (ps->dialect == RM_DIALECT_NONE) {
            if (strcmp(p, "REGEDIT4") == 0) {
                ps->dialect = RM_DIALECT_REGEDIT4;
                continue;
            }
            if (strcmp(p, "Windows Registry Editor Version 5.00") == 0) {
                ps->dialect = RM_DIALECT_V5;
                continue;
            }
            /* No header: regedit refuses the whole file. Read nothing. */
            ps->p = ps->end;
            return 0;
        }

        memset(e, 0, sizeof(*e));
        e->lineno = start;

        if (*p == '[') {
            int del = 0;
            size_t used = 0, n;
            const char *close = strrchr(p, ']');
            p++;
            if (*p == '-') {
                del = 1;
                p++;
            }
            ps->key_ok = 0;
            ps->root = rm_root(p, &used);
            if (!close || overflow || ps->root == RM_ROOT_NONE)
                continue;                   /* values under it become UNKNOWN */
            p += used;
            if (*p == '\\')
                p++;
            n = (size_t)(close - p);
            if (close < p || n >= sizeof(ps->key))
                continue;
            memcpy(ps->key, p, n);
            ps->key[n] = 0;
            ps->key_ok = 1;
            if (del) {
                ps->key_ok = 0;             /* values after a deleted key are not ours to judge */
                e->op = RM_OP_DELKEY;
                e->root = ps->root;
                memcpy(e->key, ps->key, n + 1);
                return 1;
            }
            continue;
        }

        e->op = RM_OP_UNKNOWN;
        if (ps->key_ok) {
            e->root = ps->root;
            memcpy(e->key, ps->key, strlen(ps->key) + 1);
        }
        if (overflow || !ps->key_ok)
            return 1;
        if (*p == '@') {
            p++;
            e->name[0] = 0;
        } else if (*p == '"') {
            if (rm_qstring(&p, e->name, sizeof(e->name), NULL) < 0)
                return 1;
        } else {
            return 1;
        }
        p = rm_skipws(p);
        if (*p != '=')
            return 1;
        if (rm_data(p + 1, e) < 0) {
            e->op = RM_OP_UNKNOWN;
            e->type = 0;
            e->len = 0;
        }
        return 1;
    }
}

/* ---------------------------------------------------------------------- */
/* the post-condition                                                      */
/* ---------------------------------------------------------------------- */

/*
 * Does a value read back from the registry satisfy an RM_OP_SET entry?
 *
 *   found      the value exists (RegQueryValueEx succeeded)
 *   type/data/len   what the registry returned
 *
 * Strings compare by content: Windows may or may not count the terminating
 * NUL in the length it reports (9x and NT differ), so both are accepted.
 */
static RM_UNUSED int rm_value_matches(const rm_entry_t *e, int found, unsigned type,
                                      const unsigned char *data, unsigned len)
{
    if (e->op != RM_OP_SET || !found || type != e->type)
        return 0;
    if (e->type == RM_REG_SZ) {
        unsigned n = 0;
        while (n < len && data[n])
            n++;
        return n == e->len && memcmp(data, e->data, n) == 0;
    }
    return len == e->len && (len == 0 || memcmp(data, e->data, len) == 0);
}

/* Short root name for log lines. */
static RM_UNUSED const char *rm_root_name(int root)
{
    switch (root) {
    case RM_HKLM: return "HKLM";
    case RM_HKCU: return "HKCU";
    case RM_HKCR: return "HKCR";
    case RM_HKU:  return "HKU";
    case RM_HKCC: return "HKCC";
    case RM_HKDD: return "HKDD";
    default:      return "?";
    }
}

#endif /* RETRO_REGMERGE_H */
