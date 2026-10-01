/*
 * qbinds.h - QBINDS: the fleet's WASD key layout for Quake 1 and Quake II
 * (agent 1.97.0). Design: ~/.retro-fleet/research-2026-10-01/R4_agent_design.md
 * section A.
 *
 * THE PROBLEM. Quake (1996) and Quake II (1997) ship arrow-key movement with
 * mouse look off, and each runs default.cfg -> config.cfg -> autoexec.cfg at
 * every start (Quake 1: PAK0.PAK quake.rc; Quake II: Qcommon_Init then
 * FS_ExecAutoexec). The staged Quake II autoexec.cfg carried its own WASD block
 * that OPENED WITH `unbindall` - which runs after config.cfg and re-binds only
 * ~24 keys, so the weapon keys 1-0, [ ] inventory, ENTER, the arrows, F1-F4,
 * F10, PAUSE and INS were unbound on EVERY launch (measured on .243's
 * config.cfg, 2026-09-29: no weapon keys at all). Four drifted copies of that
 * block, ctf with none at all, and no per-box off switch.
 *
 * THE DESIGN. The agent owns ONE file per Quake gamedir, FLEETKEY.CFG, and
 * makes it byte-identical to a compiled-in body for that engine (q1 / q2). The
 * library never ships FLEETKEY.CFG, so GAMESYNC (which copies only what the
 * library lists and never deletes) never touches it: one writer, no copy
 * fight, no ledger entry. The staged autoexec.cfg carries `exec fleetkey.cfg`
 * (a library change, made separately) - autoexec runs AFTER config.cfg, so the
 * fleet layout is re-applied at every game start, and a key FLEETKEY does not
 * bind keeps whatever default.cfg or the player's config.cfg gave it. The
 * agent never writes autoexec.cfg, config.cfg, default.cfg or any pak: it READS
 * autoexec.cfg to say whether the chain really reaches FLEETKEY.CFG.
 *
 * The bodies obey the three rules every Quake config here must (the validator's
 * config-quotes check, scripts/gameindex/favorites.py):
 *   - NO `;` ANYWHERE: Cbuf_Execute splits a line at a `;` outside quotes
 *     BEFORE the tokenizer strips a // comment, so text after a semicolon in a
 *     comment runs as a command (the staged autoexec's own "always run; nobody
 *     wants" prints `Unknown command "nobody"` at every start on .243);
 *   - no `"` inside a comment, and an even number of `"` on every line;
 *   - CRLF line endings, well under Quake 1's 8 KB command buffer.
 *
 * HKLM\Software\RetroAgent\QuakeBinds (DWORD):
 *   absent / 1  enforce the WASD body (the default; any other value too)
 *   0           write the NEUTRAL body - comment lines only - so the switch
 *               really turns the layout off and config.cfg decides
 *   2           hands off: FLEETKEY.CFG is not touched at all
 * HKLM\Software\RetroAgent\QuakeBindsBoot (REG_SZ): what the last pass did.
 *
 * Win32-free: tests/native/test_qbinds.c compiles exactly this, and runs the
 * real default.cfg files through the command splitter below to prove which
 * keys survive which chain.
 */
#ifndef RETRO_QBINDS_H
#define RETRO_QBINDS_H

#include <stddef.h>
#include <string.h>

#if defined(__GNUC__)
#define QB_API static __attribute__((unused))
#else
#define QB_API static
#endif

#define QB_REG_SWITCH  "QuakeBinds"
#define QB_REG_RESULT  "QuakeBindsBoot"
#define QB_FILE        "FLEETKEY.CFG"       /* 8.3: DOS QUAKE.EXE reads ID1 too */
#define QB_TMP         "FLEETKEY.TMP"
#define QB_EXEC_ARG    "fleetkey.cfg"       /* what `exec` must name, any case */
#define QB_VERSION     1
/* A FLEETKEY.CFG bigger than this is not one we wrote: it is "stale" without
 * being read whole. Every body is well under 2 KB. */
#define QB_MAX_FILE    4096
/* autoexec.cfg is read for the chain check; the staged ones are ~3.5 KB and
 * the favourites agent appends ~300 B. Anything past this is not read. */
#define QB_MAX_AUTOEXEC 16384

/* ---- the switch --------------------------------------------------------- */
enum { QB_MODE_OFF = 0, QB_MODE_WASD = 1, QB_MODE_HANDS_OFF = 2 };

/* `present` = the DWORD exists. Unknown values enforce: a typo must not turn
 * the layout off silently (the JSON reports the raw value). */
QB_API int qb_mode(int present, unsigned long value)
{
    if (!present)
        return QB_MODE_WASD;
    if (value == 0)
        return QB_MODE_OFF;
    if (value == 2)
        return QB_MODE_HANDS_OFF;
    return QB_MODE_WASD;
}

QB_API const char *qb_mode_name(int mode)
{
    return mode == QB_MODE_OFF ? "off" : mode == QB_MODE_HANDS_OFF ? "hands_off" : "wasd";
}

/* ---- the bodies ----------------------------------------------------------
 * R4 A.4, as decided by the orchestrator 2026-10-01: q2 keeps CTRL = +movedown
 * (crouch) and f = invuse; q1 leaves CTRL as default.cfg's +attack (Quake 1 has
 * no crouch, and DOS QUAKE.EXE in a Win98 DOS box may have no mouse, so a
 * keyboard fire key must remain) and binds no f. The wheel lines print a
 * harmless `"MWHEELUP" isn't a valid key` on WinQuake and DOS Quake. Q1 has no
 * cl_run cvar: cl_forwardspeed/cl_backspeed 400 is its own "always run". */
#define QB_CRLF "\r\n"

#define QB_HEAD_COMMENT \
    "// Written by the retro agent at every start - an edit here is replaced." QB_CRLF \
    "// HKLM Software RetroAgent QuakeBinds - 0 turns it off, 2 leaves this file alone." QB_CRLF

static const char qb_body_q1[] =
    "// fleetkey 1 q1" QB_CRLF
    "// The fleet key layout for Quake - WASD and mouse look." QB_CRLF
    QB_HEAD_COMMENT
    "bind w \"+forward\"" QB_CRLF
    "bind s \"+back\"" QB_CRLF
    "bind a \"+moveleft\"" QB_CRLF
    "bind d \"+moveright\"" QB_CRLF
    "bind SPACE \"+jump\"" QB_CRLF
    "bind SHIFT \"+speed\"" QB_CRLF
    "bind MOUSE1 \"+attack\"" QB_CRLF
    "bind MOUSE2 \"+jump\"" QB_CRLF
    "bind e \"impulse 10\"" QB_CRLF
    "bind q \"impulse 12\"" QB_CRLF
    "bind MWHEELUP \"impulse 10\"" QB_CRLF
    "bind MWHEELDOWN \"impulse 12\"" QB_CRLF
    "bind y \"messagemode2\"" QB_CRLF
    "+mlook" QB_CRLF
    "lookspring \"0\"" QB_CRLF
    "lookstrafe \"0\"" QB_CRLF
    "cl_forwardspeed \"400\"" QB_CRLF
    "cl_backspeed \"400\"" QB_CRLF;

static const char qb_body_q2[] =
    "// fleetkey 1 q2" QB_CRLF
    "// The fleet key layout for Quake II - WASD and mouse look." QB_CRLF
    QB_HEAD_COMMENT
    "bind w \"+forward\"" QB_CRLF
    "bind s \"+back\"" QB_CRLF
    "bind a \"+moveleft\"" QB_CRLF
    "bind d \"+moveright\"" QB_CRLF
    "bind SPACE \"+moveup\"" QB_CRLF
    "bind CTRL \"+movedown\"" QB_CRLF
    "bind SHIFT \"+speed\"" QB_CRLF
    "bind f \"invuse\"" QB_CRLF
    "bind MOUSE1 \"+attack\"" QB_CRLF
    "bind MOUSE2 \"+moveup\"" QB_CRLF
    "bind e \"weapnext\"" QB_CRLF
    "bind q \"weapprev\"" QB_CRLF
    "bind MWHEELUP \"weapnext\"" QB_CRLF
    "bind MWHEELDOWN \"weapprev\"" QB_CRLF
    "bind TAB \"inven\"" QB_CRLF
    "bind r \"cmd help\"" QB_CRLF
    "bind y \"messagemode2\"" QB_CRLF
    "bind F5 \"save quick\"" QB_CRLF
    "bind F9 \"load quick\"" QB_CRLF
    "+mlook" QB_CRLF
    "set freelook \"1\"" QB_CRLF
    "set lookspring \"0\"" QB_CRLF
    "set lookstrafe \"0\"" QB_CRLF
    "set cl_run \"1\"" QB_CRLF;

/* QuakeBinds=0: the file still exists (the autoexec's exec finds it and says
 * nothing), but it binds nothing. */
static const char qb_body_q1_off[] =
    "// fleetkey 1 q1 off" QB_CRLF
    "// QuakeBinds is 0 on this box - the fleet key layout is switched off." QB_CRLF;

static const char qb_body_q2_off[] =
    "// fleetkey 1 q2 off" QB_CRLF
    "// QuakeBinds is 0 on this box - the fleet key layout is switched off." QB_CRLF;

/* ---- profiles and titles ---------------------------------------------------
 * Data-driven so Hexen II (h2: data1) and SiN (sin: base, 2015) can be added
 * as one profile + one title row each. Scope today (orchestrator 2026-10-01):
 * Quake 1 and Quake II only. */
enum { QB_PROFILE_Q1 = 0, QB_PROFILE_Q2, QB_NPROFILES };

typedef struct {
    const char *name;           /* "q1" - in the header line and the JSON */
    const char *engine;         /* for log lines */
    const char *body;           /* the enforced bytes */
    const char *off;            /* the QuakeBinds=0 bytes */
} qb_profile_t;

static const qb_profile_t qb_profiles[QB_NPROFILES] = {
    { "q1", "Quake",    qb_body_q1, qb_body_q1_off },
    { "q2", "Quake II", qb_body_q2, qb_body_q2_off },
};

#define QB_MAX_DIRS 6

typedef struct {
    const char *title;                  /* the library directory, any case */
    int         profile;                /* QB_PROFILE_* */
    const char *dirs[QB_MAX_DIRS];      /* gamedirs a launcher reaches, NULL-terminated */
} qb_title_t;

static const qb_title_t qb_titles[] = {
    /* GLQUAKE, WINQUAKE, VOODOO\GLQUAKE (started from the title root) and DOS
     * QUAKE.EXE all read ID1. */
    { "Quake1",         QB_PROFILE_Q1, { "ID1", NULL } },
    /* .243's lane (Win98, the 3dfx MiniGL). */
    { "Quake2Win9x",    QB_PROFILE_Q2, { "baseq2", NULL } },
    /* Quake II runs only the CURRENT gamedir's autoexec.cfg, so each mission
     * pack and ThreeWave CTF needs its own FLEETKEY.CFG beside its own exec. */
    { "Quake2Complete", QB_PROFILE_Q2, { "baseq2", "ctf", "rogue", "xatrix", NULL } },
};
#define QB_NTITLES ((int)(sizeof(qb_titles) / sizeof(qb_titles[0])))

QB_API int qb_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

QB_API int qb_streq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (qb_lower((unsigned char)*a) != qb_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* The title row for a library directory name (case-insensitive), or NULL. */
QB_API const qb_title_t *qb_title_find(const char *title)
{
    int i;
    if (!title)
        return NULL;
    for (i = 0; i < QB_NTITLES; i++)
        if (qb_streq_ci(qb_titles[i].title, title))
            return &qb_titles[i];
    return NULL;
}

/* The exact bytes FLEETKEY.CFG must hold for this profile and mode. NULL in
 * hands-off mode (nothing is written) or for an unknown profile. */
QB_API const char *qb_body(int profile, int mode, size_t *len)
{
    const char *b;
    if (len)
        *len = 0;
    if (profile < 0 || profile >= QB_NPROFILES || mode == QB_MODE_HANDS_OFF)
        return NULL;
    b = mode == QB_MODE_OFF ? qb_profiles[profile].off : qb_profiles[profile].body;
    if (len)
        *len = strlen(b);
    return b;
}

/* ---- the file on the box ------------------------------------------------ */
enum { QB_FILE_MISSING = 0, QB_FILE_CURRENT, QB_FILE_STALE };

/* Byte-exact: anything but the compiled-in body is stale, a trailing space
 * or an LF-only line ending included. */
QB_API int qb_file_state(int have, const char *cur, size_t len, const char *want, size_t wlen)
{
    if (!have)
        return QB_FILE_MISSING;
    if (len == wlen && (len == 0 || memcmp(cur, want, len) == 0))
        return QB_FILE_CURRENT;
    return QB_FILE_STALE;
}

/* ---- Quake's own command splitting --------------------------------------
 * Cbuf_Execute (Quake 1 and II alike) takes text up to a '\n', or up to a ';'
 * that is outside double quotes - the quote count restarts at every command and
 * the '\n' always ends one. Cmd_TokenizeString / COM_Parse then splits that
 * command: whitespace is any byte <= ' ', `//` comments out the rest of the
 * command, a "..." is one token. A ';' inside a // comment therefore still
 * ends the command, and what follows it runs (E10). */
typedef struct {
    const char *p, *end;
    int  line;          /* 1-based physical line of the NEXT command */
} qb_cmdit_t;

QB_API void qb_cmdit_init(qb_cmdit_t *it, const char *text, size_t len)
{
    it->p = text;
    it->end = text ? text + len : text;
    it->line = 1;
}

/* The next command's raw text into buf (truncated to bufsz-1). *line gets the
 * physical line it began on. Returns 0 at the end of the text. */
QB_API int qb_cmdit_next(qb_cmdit_t *it, char *buf, size_t bufsz, int *line)
{
    int quotes = 0;
    size_t n = 0;
    if (!it->p || it->p >= it->end)
        return 0;
    if (line)
        *line = it->line;
    while (it->p < it->end) {
        char c = *it->p++;
        if (c == '\n') {
            it->line++;
            break;
        }
        if (c == '"')
            quotes++;
        if (!(quotes & 1) && c == ';')
            break;
        if (n + 1 < bufsz)
            buf[n++] = c;
    }
    if (bufsz)
        buf[n < bufsz ? n : bufsz - 1] = 0;
    return 1;
}

#define QB_MAX_ARGS 8
#define QB_ARG_LEN  64

/* COM_Parse over one command. Returns argc (0 = empty or comment only). */
QB_API int qb_tokenize(const char *cmd, char argv[QB_MAX_ARGS][QB_ARG_LEN])
{
    int argc = 0;
    const char *p = cmd;
    for (;;) {
        size_t n = 0;
        while (*p && (unsigned char)*p <= ' ')
            p++;
        if (!*p)
            break;
        if (p[0] == '/' && p[1] == '/')
            break;                      /* the rest of the command is a comment */
        if (argc >= QB_MAX_ARGS)
            break;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (n + 1 < QB_ARG_LEN)
                    argv[argc][n++] = *p;
                p++;
            }
            if (*p == '"')
                p++;
        } else {
            while (*p && (unsigned char)*p > ' ') {
                if (n + 1 < QB_ARG_LEN)
                    argv[argc][n++] = *p;
                p++;
            }
        }
        argv[argc][n] = 0;
        argc++;
    }
    return argc;
}

/* ---- the keys a body binds ------------------------------------------------ */
#define QB_MAX_KEYS 32

/* Every key the body binds with `bind <key> <command>`, in order. */
QB_API int qb_body_keys(const char *body, char keys[QB_MAX_KEYS][QB_ARG_LEN])
{
    qb_cmdit_t it;
    char cmd[512];
    char argv[QB_MAX_ARGS][QB_ARG_LEN];
    int n = 0, line;
    qb_cmdit_init(&it, body, strlen(body));
    while (qb_cmdit_next(&it, cmd, sizeof(cmd), &line)) {
        int argc = qb_tokenize(cmd, argv);
        if (argc >= 3 && qb_streq_ci(argv[0], "bind") && n < QB_MAX_KEYS) {
            memcpy(keys[n], argv[1], QB_ARG_LEN);
            n++;
        }
    }
    return n;
}

/* ---- does autoexec.cfg reach FLEETKEY.CFG? ------------------------------- */
enum { QB_UB_NONE = 0, QB_UB_BEFORE, QB_UB_AFTER };

QB_API const char *qb_unbindall_name(int v)
{
    return v == QB_UB_BEFORE ? "before" : v == QB_UB_AFTER ? "after" : "none";
}

#define QB_MAX_REBINDS 8

typedef struct {
    int  exec_live;         /* a live `exec fleetkey.cfg` (any case, quoted or not) */
    int  exec_line;         /* its 1-based physical line, 0 = none */
    int  exec_commented;    /* named only inside a // comment */
    int  unbindall;         /* QB_UB_*: AFTER wins over BEFORE */
    int  quote_trap;        /* a line BEFORE the exec has an odd number of quotes */
    int  quote_line;        /* the first such line */
    int  nrebind;           /* fleet keys bound or unbound AFTER the exec */
    char rebinds[QB_MAX_REBINDS][QB_ARG_LEN];
} qb_scan_t;

QB_API int qb_has_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < n; i++)
            if (qb_lower((unsigned char)hay[i]) != qb_lower((unsigned char)needle[i]))
                break;
        if (i == n)
            return 1;
    }
    return 0;
}

/* Scan autoexec.cfg the way the engine runs it, for this profile's keys. */
QB_API void qb_autoexec_scan(const char *text, size_t len, int profile, qb_scan_t *s)
{
    qb_cmdit_t it;
    char cmd[1024];
    char argv[QB_MAX_ARGS][QB_ARG_LEN];
    char keys[QB_MAX_KEYS][QB_ARG_LEN];
    int nkeys = 0, line, i;
    const char *lp;

    memset(s, 0, sizeof(*s));
    if (!text)
        return;
    if (profile >= 0 && profile < QB_NPROFILES)
        nkeys = qb_body_keys(qb_profiles[profile].body, keys);

    qb_cmdit_init(&it, text, len);
    while (qb_cmdit_next(&it, cmd, sizeof(cmd), &line)) {
        int argc = qb_tokenize(cmd, argv);
        if (argc < 1)
            continue;
        if (qb_streq_ci(argv[0], "exec") && argc >= 2 && qb_streq_ci(argv[1], QB_EXEC_ARG)) {
            if (!s->exec_live) {
                s->exec_live = 1;
                s->exec_line = line;
            }
            continue;
        }
        if (qb_streq_ci(argv[0], "unbindall")) {
            if (s->exec_live)
                s->unbindall = QB_UB_AFTER;
            else if (s->unbindall == QB_UB_NONE)
                s->unbindall = QB_UB_BEFORE;
            continue;
        }
        if (!s->exec_live)
            continue;
        if ((qb_streq_ci(argv[0], "bind") && argc >= 3) ||
            (qb_streq_ci(argv[0], "unbind") && argc >= 2)) {
            for (i = 0; i < nkeys; i++)
                if (qb_streq_ci(argv[1], keys[i]))
                    break;
            if (i < nkeys && s->nrebind < QB_MAX_REBINDS) {
                memcpy(s->rebinds[s->nrebind], keys[i], QB_ARG_LEN);
                s->nrebind++;
            }
        }
    }

    /* Physical-line checks: a quote left open before the exec (the validator's
     * config-quotes trap - an engine that lets the string run on swallows the
     * lines after it, the exec included), and an exec that exists only as
     * comment text. */
    lp = text;
    line = 1;
    while (lp < text + len) {
        const char *e = lp;
        int q = 0;
        while (e < text + len && *e != '\n') {
            if (*e == '"')
                q++;
            e++;
        }
        if ((q & 1) && (!s->exec_live || line < s->exec_line) && !s->quote_trap) {
            s->quote_trap = 1;
            s->quote_line = line;
        }
        if (!s->exec_live) {
            const char *c;
            for (c = lp; c + 1 < e; c++)
                if (c[0] == '/' && c[1] == '/')
                    break;
            if (c + 1 < e) {
                char tail[256];
                size_t n = (size_t)(e - c);
                if (n >= sizeof(tail))
                    n = sizeof(tail) - 1;
                memcpy(tail, c, n);
                tail[n] = 0;
                if (qb_has_ci(tail, "exec") && qb_has_ci(tail, QB_EXEC_ARG))
                    s->exec_commented = 1;
            }
        }
        lp = e < text + len ? e + 1 : e;
        line++;
    }
}

/* The fleet layout is in force at every launch of this gamedir: the WASD body
 * is what FLEETKEY.CFG holds NOW (whatever the mode - with QuakeBinds=0 it holds
 * the neutral body and this is rightly false), autoexec.cfg execs it, nothing
 * after the exec undoes it, and no open quote can swallow the exec line. */
QB_API int qb_effective(int wasd_in_place, const qb_scan_t *s)
{
    return wasd_in_place && s->exec_live && s->unbindall != QB_UB_AFTER &&
           !s->quote_trap && s->nrebind == 0;
}

/* The exec column of the report. */
QB_API const char *qb_exec_name(int have_autoexec, const qb_scan_t *s)
{
    if (!have_autoexec)
        return "no_autoexec";
    if (s->exec_live)
        return "present";
    if (s->exec_commented)
        return "commented";
    return "missing";
}

#endif /* RETRO_QBINDS_H */
