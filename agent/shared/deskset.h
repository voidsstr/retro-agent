/*
 * deskset.h - which desktop shortcuts a GAMESYNC run may take away, and did the
 * SET of desktop icons change? (agent 1.90.0, "sweep at the END")
 *
 * THE DEFECT THIS FIXES (.110, XP P4, 2026-09-28). gs_run() used to begin by
 * moving EVERY .lnk/.pif/.url off both desktops into C:\retro-desktop-backup
 * and then rebuild the icons title by title as the copy loop reached each one.
 * A run on .110 then sat in state=sizing ("enumerating library") for ~100
 * minutes - a minimized ioquake3 held 96% of the single CPU and GAMESYNC runs
 * at THREAD_PRIORITY_IDLE by design - and for all of that time the desktop
 * showed two icons (Retro Agent, Retro Chat) where 97 had been. Nothing said
 * why. The same window opened on every run that failed before the copy loop
 * (library unreachable - retried every two minutes, each retry sweeping again),
 * was aborted, or was killed with the agent: the desktop stayed empty until a
 * LATER run completed.
 *
 * THE FIX: nothing is removed until the run has finished deciding. The run
 * writes each shortcut over the existing file in place, and only at the end
 * does it move away what was on the desktop when it STARTED and was neither
 * rewritten nor confirmed by this run. The final desktop is the same one the
 * old sweep-first order produced; the difference is that no icon the run is
 * going to put back is ever missing in between, and a run that does not get to
 * the end removes nothing at all.
 *
 * WHAT MAY BE SWEPT, precisely: an entry sampled BEFORE the run touched the
 * desktop, on a desktop this run did not write it to. So:
 *   - a shortcut this run rewrote (or found already correct) stays;
 *   - a shortcut that appeared DURING the run, by anyone, stays - it was not
 *     in the sample, exactly as the old order left it (it had swept first);
 *   - a duplicate on the per-user desktop of an icon rewritten on All Users is
 *     swept, as before, so one icon does not become two;
 *   - past DS_MAX sampled names the rest are left alone, never guessed at.
 *
 * THE ICON-REBUILD GATE (CLAUDE.md "The icon layout is rebuilt ONLY when the
 * desktop changed") is answered from the same sample: icons ADDED (written by
 * this run, not on any desktop at the start) plus icons GONE (every copy that
 * was there at the start was really moved away, and the run did not put the
 * name back). A box that rewrites the same 81 icons still reports 0.
 *
 * NAMES ARE COMPARED WHOLE. The previous snapshot truncated to 95 characters;
 * harmless while it only fed a counter, fatal once it decides what to remove -
 * a long display name would never match its own rewrite and the run would
 * sweep the icon it had just written. DS_NAME is MAX_PATH, and a name that
 * cannot be stored whole marks the sample incomplete instead.
 *
 * Win32-free so tests/native/test_desk_sweep_order.c compiles THIS file.
 */
#ifndef RETRO_DESKSET_H
#define RETRO_DESKSET_H

#ifdef __GNUC__
#define DS_UNUSED __attribute__((unused))
#else
#define DS_UNUSED
#endif

#define DS_MAX     256
#define DS_NAME    260          /* MAX_PATH - a shortcut name is never longer */

/* Which desktop a copy of an icon lives on. */
#define DS_COMMON  1u           /* All Users - where the agent writes          */
#define DS_USER    2u           /* the logged-on user's own Desktop            */

typedef struct {
    char          name[DS_NAME];
    unsigned char at;           /* desktops it was on when the run started     */
    unsigned char kept;         /* desktops this run wrote it to / confirmed   */
    unsigned char gone;         /* desktops the end-of-run sweep cleared       */
} ds_entry_t;

typedef struct {
    ds_entry_t e[DS_MAX];
    int        n;
    int        overflow;        /* something on the desktop was NOT sampled    */
    long       added;           /* written this run, on no desktop at start    */
} ds_set_t;

DS_UNUSED static char ds_fold(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Windows filenames are case-insensitive, and a Save over an existing file
 * keeps the case the file was first created with. */
DS_UNUSED static int ds_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (ds_fold(*a) != ds_fold(*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* The three kinds of file the desktop sweep has always taken - and only
 * those: a real file somebody left on the desktop is never touched. */
DS_UNUSED static int ds_is_sweepable(const char *name)
{
    const char *dot = 0, *p;
    for (p = name; *p; p++)
        if (*p == '.')
            dot = p;
    if (!dot)
        return 0;
    return ds_ieq(dot, ".lnk") || ds_ieq(dot, ".pif") || ds_ieq(dot, ".url");
}

DS_UNUSED static void ds_reset(ds_set_t *s)
{
    s->n = 0;
    s->overflow = 0;
    s->added = 0;
}

/* The name a shortcut REALLY has on the desktop. Windows 9x's shell saves a
 * shortcut to an MS-DOS program - a .bat or a DOS .exe - as "<name>.pif", not
 * the "<name>.lnk" it was asked to save (agent 1.93.1). Claiming the .lnk name
 * left the .pif - sampled at the start, rewritten by this very run - unclaimed,
 * and the end-of-run sweep moved EVERY game icon off .243's desktop into
 * C:\retro-desktop-backup. `lnk_exists`/`pif_exists` are what the caller
 * found on disk; `out` gets the name to claim. */
DS_UNUSED static const char *ds_written_name(const char *lnk_name, int lnk_exists, int pif_exists,
                                             char *out, size_t cap)
{
    size_t n = 0;
    while (lnk_name[n]) n++;
    if (lnk_exists || !pif_exists || n < 5 || n >= cap || !ds_ieq(lnk_name + n - 4, ".lnk"))
        return lnk_name;
    for (n = 0; lnk_name[n]; n++) out[n] = lnk_name[n];
    out[n - 3] = 'p'; out[n - 2] = 'i'; out[n - 1] = 'f'; out[n] = 0;
    return out;
}

DS_UNUSED static int ds_find(const ds_set_t *s, const char *name)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (ds_ieq(s->e[i].name, name))
            return i;
    return -1;
}

/* Sample one file seen on desktop `where` BEFORE the run touches anything. */
DS_UNUSED static void ds_add(ds_set_t *s, const char *name, unsigned where)
{
    int i = 0, len = 0;
    if (!ds_is_sweepable(name))
        return;
    i = ds_find(s, name);
    if (i >= 0) {                       /* the same name on both desktops */
        s->e[i].at |= (unsigned char)where;
        return;
    }
    while (name[len])
        len++;
    if (s->n >= DS_MAX || len >= DS_NAME) {
        s->overflow = 1;                /* never sweep what we could not see */
        return;
    }
    for (i = 0; i <= len; i++)
        s->e[s->n].name[i] = name[i];
    s->e[s->n].at = (unsigned char)where;
    s->e[s->n].kept = 0;
    s->e[s->n].gone = 0;
    s->n++;
}

/* This run wrote `name` to desktop `where` (written=1), or found it there
 * already correct and left it (written=0). Either way that copy STAYS.
 * Returns 1 when the write put a NEW icon on the desktop - the only kind of
 * write the rebuild gate counts, because every existing icon is rewritten on
 * every pass and counting writes would be true on every run. */
DS_UNUSED static int ds_claim(ds_set_t *s, const char *name, unsigned where,
                              int written)
{
    int i = ds_find(s, name);
    if (i >= 0) {
        s->e[i].kept |= (unsigned char)where;
        return 0;
    }
    if (!written)
        return 0;       /* confirmed an icon it did not sample: no change made */
    if (s->overflow)
        return 0;       /* it may be one of the unsampled ones: invent nothing */
    s->added++;
    return 1;
}

/* The copies of entry `e` the end-of-run sweep should move away. */
DS_UNUSED static unsigned ds_sweep_bits(const ds_entry_t *e)
{
    return (unsigned)(e->at & ~e->kept & ~e->gone) & (DS_COMMON | DS_USER);
}

/* The sweep moved (or found already absent) the copy on desktop `where`. */
DS_UNUSED static void ds_mark_gone(ds_entry_t *e, unsigned where)
{
    e->gone |= (unsigned char)where;
}

/* May this run sweep at all? Only one that got to the end having considered
 * EVERY title. An aborted run, or one whose library listing was cut short or
 * capped, never looked at some titles - and sweeping would take the icons of
 * exactly those titles away, installed games included. Leaving clutter is the
 * cheap failure; an empty desktop is the one this file exists to prevent. */
DS_UNUSED static int ds_run_may_sweep(int aborted, int listing_complete)
{
    return !aborted && listing_complete;
}

/* Net change in the SET of desktop icons: added, plus names that were there at
 * the start, were not put back, and whose every copy really went. */
DS_UNUSED static long ds_changed(const ds_set_t *s)
{
    long gone = 0;
    int  i;
    for (i = 0; i < s->n; i++) {
        const ds_entry_t *e = &s->e[i];
        if (e->at && !e->kept && (e->gone & e->at) == e->at)
            gone++;
    }
    return s->added + gone;
}

#endif /* RETRO_DESKSET_H */
