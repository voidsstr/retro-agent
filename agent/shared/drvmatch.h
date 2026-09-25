/*
 * drvmatch.h - which INFs in the staged driver tree (C:\D) could serve a device.
 *
 * Header-only and free of Win32, so the code the agent runs is the code
 * tests/native/test_drvmatch.c compiles. SetupAPI stays in agent/src/gamesync.c,
 * which also asks Windows itself to confirm each candidate before installing it
 * (gs_inf_serves); this header is the cheap prefilter that finds the candidates.
 *
 * THE BUG THIS REPLACES (agent <= 1.85.0). gs_find_inf_for() was handed the
 * device's hardware-id REG_MULTI_SZ as a plain C string - so only its FIRST id,
 * the most specific one, e.g.
 *
 *     PCI\VEN_8086&DEV_2572&SUBSYS_01741028&REV_02
 *
 * - and strstr()'d that into each INF. INFs almost never name a revision, so it
 * matched nothing, on every machine, and two things failed silently:
 *
 *   - gs_install_missing_drivers() never installed a thing. The Dell Dimension
 *     3000 it was written for (865G display, AC'97 audio) would have stayed at
 *     640x480 VGA with its drivers sitting in C:\D.
 *   - gs_devices_unconfigured() uses the same lookup to decide whether C:\D is
 *     still needed, so it concluded "nothing in C:\D serves it" for exactly the
 *     devices C:\D serves - and the reclaim DELETED the tree.
 *
 * WHAT A FIRST FIX GOT WRONG (caught in review against the real 3,669-INF tree,
 * before release): matching every id as a whole token ANYWHERE in the INF. The
 * tree is DriverPacks - it is full of ids in ';' comments, ExcludeFromSelect,
 * [*.PosDup], AddReg strings and models sections XP never reads. That picked
 * T027\stac97.inf for Dell ICH6 audio via a commented-out line, forced the
 * generic C-Media driver onto a TerraTec card whose SUBSYS line is commented
 * out, and "found" a driver for a COM port in an Intel ME INF's PosDup list -
 * which would have pinned 2.4 GB of C:\D on every boot. About 600 ids resolved
 * to an INF that does not serve them. So:
 *
 *   1. drvmatch_prepare() reduces an INF to the ID FIELDS OF ITS XP-x86 MODEL
 *      LINES and blanks everything else - comments, strings, other sections,
 *      and models sections decorated for another OS/architecture.
 *   2. Candidate ids are the device's hardware ids, then compatible ids that
 *      still name a DEVICE (PCI/HDAUDIO "&DEV_", USB "&PID_"). Family ids
 *      ("*PNP0501", "USB\ROOT_HUB", "PCI\CC_0300") never pick a driver: a
 *      FORCE install on one reaches every device that shares it.
 *   3. Candidates are ranked most-specific-id first, tree order second, and
 *      the agent keeps several, so a failed install falls through to the next.
 *
 * Ids are matched as whole TOKENS: "PCI\VEN_8086&DEV_24D5" must not be found
 * inside "PCI\VEN_8086&DEV_24D5&SUBSYS_100115BD", an INF for one Realtek board.
 */
#ifndef RETRO_DRVMATCH_H
#define RETRO_DRVMATCH_H

#include <string.h>

#define DRVMATCH_MAX_IDS     32
#define DRVMATCH_MAX_MODELS  128
#define DRVMATCH_MAX_STRSECT 8
#define DRVMATCH_NAME_MAX    96

/* Characters that can appear inside a device id. Callers upper-case. */
static int drvmatch_idch(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '&' || c == '\\' || c == '.' || c == '*' || c == '-';
}

/* Is `id` present in `buf` as a whole token? Both upper-case. */
static int drvmatch_has_token(const char *buf, const char *id)
{
    size_t      n = strlen(id);
    const char *p = buf;

    if (!n)
        return 0;
    while ((p = strstr(p, id)) != NULL) {
        int before = (p == buf) || !drvmatch_idch(p[-1]);
        int after  = !drvmatch_idch(p[n]);
        if (before && after)
            return 1;
        p++;
    }
    return 0;
}

static int drvmatch_prefix(const char *s, const char *pre)
{
    return strncmp(s, pre, strlen(pre)) == 0;
}

/* A FAMILY id names a kind of device, not a device. Microsoft's generic PnP
 * ids, class-only ids, root hubs, composite parents. Upper-case input. */
static int drvmatch_is_family(const char *id)
{
    if (drvmatch_prefix(id, "*PNP") || drvmatch_prefix(id, "ACPI\\PNP") ||
        drvmatch_prefix(id, "ACPI\\*PNP") || drvmatch_prefix(id, "HID_DEVICE") ||
        drvmatch_prefix(id, "PCI\\CC_") || drvmatch_prefix(id, "USB\\CLASS_") ||
        drvmatch_prefix(id, "USB\\COMPOSITE"))
        return 1;
    /* Only the BARE root-hub ids are families; "USB\ROOT_HUB&VID8086&PID24D2"
     * names one controller's hub. */
    return strcmp(id, "USB\\ROOT_HUB") == 0 || strcmp(id, "USB\\ROOT_HUB20") == 0;
}

/* May this id be used to pick - and force-install - a driver?
 *
 * A hardware id may, unless it is a family id (USB\ROOT_HUB is the third
 * HARDWARE id of every USB 1.1 root hub; *PNP0501 belongs to every COM port).
 * A compatible id may only when it still names the DEVICE: PCI and HD Audio
 * "&DEV_", USB "&PID_". HD Audio matters here - a codec's VEN/DEV-level id is
 * a COMPATIBLE id and generic HDA INFs (Realtek HDARt.inf, SigmaTel STHDA.INF)
 * name only that level. */
static int drvmatch_usable(const char *id, int is_compat)
{
    if (!id || !id[0] || drvmatch_is_family(id))
        return 0;
    if (!is_compat)
        return 1;
    return strstr(id, "&DEV_") != NULL || strstr(id, "&PID_") != NULL;
}

/* Collect the candidate ids from a hardware-id and a compatible-id MULTI_SZ
 * (either may be NULL), in priority order, into out[]. Both lists must already
 * be upper-case. Returns the count. */
static int drvmatch_collect(const char *hw, const char *compat,
                            const char **out, int max)
{
    int         n = 0, pass;
    const char *p;

    for (pass = 0; pass < 2; pass++) {
        p = pass ? compat : hw;
        if (!p)
            continue;
        for (; *p && n < max; p += strlen(p) + 1)
            if (drvmatch_usable(p, pass))
                out[n++] = p;
    }
    return n;
}

/* Fold a UTF-16LE INF (it starts FF FE) to 8-bit in place. Device ids are
 * ASCII, so keeping the low byte of each unit is exact for everything compared;
 * anything else becomes '?', which is not an id character. Returns the new
 * length; the buffer is NUL-terminated there. An ANSI buffer is left alone.
 *
 * Twenty-odd INFs in the tree are UTF-16 - Realtek and Marvell NICs, SigmaTel
 * HD Audio, Intel HDMI - and the old reader strstr()'d them as ANSI, where the
 * first 00 byte ends the string. */
static size_t drvmatch_fold_utf16(char *buf, size_t len)
{
    size_t i, o = 0;

    if (len < 2 || (unsigned char)buf[0] != 0xFF || (unsigned char)buf[1] != 0xFE)
        return len;
    for (i = 2; i + 1 < len; i += 2) {
        unsigned char lo = (unsigned char)buf[i], hi = (unsigned char)buf[i + 1];
        buf[o++] = (char)(hi ? '?' : (lo ? lo : ' '));
    }
    buf[o] = 0;
    return o;
}

/* ---- INF structure ---------------------------------------------------- */

static int drvmatch_space(char c)
{
    return c == ' ' || c == '\t';
}

/* Blank [a, b) with spaces, preserving line breaks. */
static void drvmatch_blank(char *a, char *b)
{
    for (; a < b; a++)
        if (*a != '\n' && *a != '\r')
            *a = ' ';
}

/* Copy the trimmed text [a, b) to out (upper-case already), max-1 chars. */
static void drvmatch_trim_copy(const char *a, const char *b, char *out, size_t max)
{
    size_t n;
    while (a < b && (drvmatch_space(*a) || *a == '"'))
        a++;
    while (b > a && (drvmatch_space(b[-1]) || b[-1] == '"' || b[-1] == '\r' ||
                     b[-1] == '\n'))
        b--;
    n = (size_t)(b - a);
    if (n >= max)
        n = max - 1;
    memcpy(out, a, n);
    out[n] = 0;
}

/* Does a [Manufacturer] TargetOSVersion decoration apply to 32-bit XP (NT 5.1)?
 * "NT", "NTX86", "NT.5.1", "NTX86.5", "NTX86.5.1" do; "NTAMD64", "NTIA64",
 * "NTX86.6.0", "NT.5.2" do not. Anything after the version (product type,
 * suite mask) is ignored. */
static int drvmatch_deco_applies(const char *d)
{
    int major = 0, minor = 0;

    if (!drvmatch_prefix(d, "NT"))
        return 0;
    d += 2;
    if (drvmatch_prefix(d, "X86"))
        d += 3;
    else if (*d && *d != '.')
        return 0;                       /* NTAMD64, NTIA64, NTARM ... */
    if (*d != '.')
        return 1;                       /* no version: every NT */
    d++;
    /* clamped: a malformed 20-digit version must not overflow an int */
    while (*d >= '0' && *d <= '9') {
        if (major < 10000)
            major = major * 10 + (*d - '0');
        d++;
    }
    if (*d == '.') {
        d++;
        while (*d >= '0' && *d <= '9') {
            if (minor < 10000)
                minor = minor * 10 + (*d - '0');
            d++;
        }
    }
    return major < 5 || (major == 5 && minor <= 1);
}

/* Reduce an upper-cased INF, in place, to the id fields of the model lines XP
 * x86 would read, blanking everything else. Returns the number of models
 * sections found. After this, drvmatch_has_token() only sees real model ids.
 *
 *   - ';' starts a comment unless inside quotes.
 *   - [Manufacturer] names the models sections: "%MFG% = SECT[,DECO...]". If
 *     any DECO applies to XP x86 the section read is SECT.DECO (each applicable
 *     one is kept - Windows picks one of them); otherwise the undecorated SECT.
 *   - In a models section, "desc = install-section, id[, id...]" - only the text
 *     after the first comma following the '=' is an id field.
 *   - An id field may be a %KEY% defined in [Strings] (Matrox's H010 INFs do
 *     this for every model). When any kept id field contains '%', the VALUES of
 *     the [Strings*] sections stay visible too, so the expanded id can match.
 *   - More [Manufacturer] entries than DRVMATCH_MAX_MODELS (I010\fgl23mon.inf
 *     names 90) must not silently hide the rest: on overflow every section that
 *     is not obviously something else is read as a models section. The on-box
 *     check (gs_inf_serves) filters whatever that over-offers. */
static int drvmatch_is_nonmodel_section(const char *name)
{
    return strcmp(name, "MANUFACTURER") == 0 || strcmp(name, "VERSION") == 0 ||
           drvmatch_prefix(name, "STRINGS") || strcmp(name, "CONTROLFLAGS") == 0 ||
           drvmatch_prefix(name, "SOURCEDISKS") || strstr(name, ".POSDUP") != NULL;
}

static int drvmatch_prepare(char *buf)
{
    char  models[DRVMATCH_MAX_MODELS][DRVMATCH_NAME_MAX];
    char *strsect[DRVMATCH_MAX_STRSECT][2];
    int   nmodels = 0, pass, overflow = 0, nstr = 0, strkey = 0, in_str = 0;
    char *line, *end;

    /* Pass 0: strip comments (quote-aware), and collect [Manufacturer]. */
    for (pass = 0; pass < 2; pass++) {
        int in_mfg = 0, in_models = 0;
        for (line = buf; *line; line = end) {
            char *p, *eq = NULL, *comma = NULL;
            int   q = 0;

            end = strchr(line, '\n');
            end = end ? end + 1 : line + strlen(line);

            if (pass == 0) {
                for (p = line; p < end; p++) {
                    if (*p == '"')
                        q = !q;
                    else if (*p == ';' && !q) {
                        drvmatch_blank(p, end);
                        break;
                    }
                }
            }
            p = line;
            while (p < end && drvmatch_space(*p))
                p++;
            if (*p == '[') {
                char  name[DRVMATCH_NAME_MAX];
                char *close = p + 1;
                while (close < end && *close != ']')
                    close++;
                drvmatch_trim_copy(p + 1, close, name, sizeof(name));
                in_mfg = strcmp(name, "MANUFACTURER") == 0;
                in_models = 0;
                if (pass == 1) {
                    int i;
                    for (i = 0; i < nmodels; i++)
                        if (strcmp(models[i], name) == 0)
                            in_models = 1;
                    if (overflow && !drvmatch_is_nonmodel_section(name))
                        in_models = 1;
                    if (in_str)                     /* close the open range */
                        strsect[nstr - 1][1] = line;
                    in_str = 0;
                    if (drvmatch_prefix(name, "STRINGS") && nstr < DRVMATCH_MAX_STRSECT) {
                        strsect[nstr][0] = end;
                        strsect[nstr][1] = end;
                        nstr++;
                        in_str = 1;
                    }
                    drvmatch_blank(line, end);
                }
                continue;
            }
            if (pass == 0) {
                if (in_mfg && nmodels < DRVMATCH_MAX_MODELS - 1) {
                    /* value side: after '=' if present, else the whole line */
                    char *v = memchr(line, '=', (size_t)(end - line));
                    char  sect[DRVMATCH_NAME_MAX], deco[DRVMATCH_NAME_MAX];
                    char *f, *next;
                    int   any = 0, first = 1;

                    v = v ? v + 1 : line;
                    sect[0] = 0;
                    for (f = v; f < end; f = next) {
                        next = memchr(f, ',', (size_t)(end - f));
                        next = next ? next + 1 : end;
                        if (first) {
                            drvmatch_trim_copy(f, next == end ? end : next - 1,
                                               sect, sizeof(sect));
                            first = 0;
                            continue;
                        }
                        drvmatch_trim_copy(f, next == end ? end : next - 1,
                                           deco, sizeof(deco));
                        if (deco[0] && drvmatch_deco_applies(deco) &&
                            nmodels < DRVMATCH_MAX_MODELS &&
                            strlen(sect) + 1 + strlen(deco) < DRVMATCH_NAME_MAX) {
                            strcpy(models[nmodels], sect);
                            strcat(models[nmodels], ".");
                            strcat(models[nmodels], deco);
                            nmodels++;
                            any = 1;
                        }
                    }
                    if (sect[0] && !any && nmodels < DRVMATCH_MAX_MODELS)
                        strcpy(models[nmodels++], sect);
                } else if (in_mfg) {
                    overflow = 1;       /* more entries than we can hold */
                }
                continue;
            }
            /* pass 1: keep only the id fields of model lines */
            if (in_str) {
                /* [Strings] "KEY = value": decided after the whole file (see
                 * strkey below). The range is blanked at the end unless needed. */
                continue;
            }
            if (!in_models) {
                drvmatch_blank(line, end);
                continue;
            }
            q = 0;
            for (p = line; p < end; p++) {
                if (*p == '"')
                    q = !q;
                else if (!q && *p == '=' && !eq)
                    eq = p;
                else if (!q && *p == ',' && eq) {
                    comma = p;
                    break;
                }
            }
            if (!comma) {
                drvmatch_blank(line, end);
            } else {
                drvmatch_blank(line, comma + 1);
                if (memchr(comma + 1, '%', (size_t)(end - comma - 1)))
                    strkey = 1;
            }
        }
    }
    if (in_str)
        strsect[nstr - 1][1] = buf + strlen(buf);
    {
        int i;
        for (i = 0; i < nstr; i++) {
            char *a = strsect[i][0], *b = strsect[i][1];
            if (!strkey) {
                drvmatch_blank(a, b);
                continue;
            }
            /* keep each value, blank its key: "KEY = value" -> "      value" */
            for (line = a; line < b; line = end) {
                char *eq;
                end = memchr(line, '\n', (size_t)(b - line));
                end = end ? end + 1 : b;
                eq = memchr(line, '=', (size_t)(end - line));
                drvmatch_blank(line, eq ? eq + 1 : end);
            }
        }
    }
    return nmodels;
}

/* The most specific of ids[0..n) that a PREPARED INF names, as its index, or -1. */
static int drvmatch_best(const char *buf, const char *const *ids, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (drvmatch_has_token(buf, ids[i]))
            return i;
    return -1;
}

/* ---- the payload an INF needs ------------------------------------------ */

/* Call cb(relpath, ctx) for every file an upper-cased RAW (not prepared) INF
 * declares in [SourceDisksFiles] / [SourceDisksFiles.x86], as a path relative to
 * the INF's own directory: "<SourceDisksNames path>\<subdir>\<file>". Returns the
 * number of files listed; 0 means the INF declares none (a NO_DRV chipset INF,
 * or one that relies on files already in Windows) and there is nothing to check.
 *
 * WHY. Windows confirming that an INF names a device says nothing about whether
 * the INF's FILES are there, and in this image 84 INFs' are not - every ATI
 * display INF points at a .\B136646-style subdirectory that inject-drivers.sh
 * never copied. On XP, UpdateDriverForPlugAndPlayDevices has no non-interactive
 * mode, so forcing such an INF raises a "Files Needed" prompt on the console and
 * blocks the gamesync thread until someone at the keyboard answers it. */
typedef void (*drvmatch_file_cb)(const char *relpath, void *ctx);

#define DRVMATCH_MAX_DISKS 16

static int drvmatch_payload(const char *buf, drvmatch_file_cb cb, void *ctx)
{
    char        diskid[DRVMATCH_MAX_DISKS][16], diskpath[DRVMATCH_MAX_DISKS][128];
    char        diskcab[DRVMATCH_MAX_DISKS][64];
    int         cab_reported[DRVMATCH_MAX_DISKS];
    int         ndisk = 0, pass, nfiles = 0;
    const char *line, *end;

    for (pass = 0; pass < 2; pass++) {
        int in = 0;
        for (line = buf; *line; line = end) {
            const char *p, *stop;
            char        name[DRVMATCH_NAME_MAX], rawline[512];
            size_t      n;
            int         q = 0;

            end = strchr(line, '\n');
            end = end ? end + 1 : line + strlen(line);
            /* copy the line without its comment */
            for (p = line, n = 0; p < end && n < sizeof(rawline) - 1; p++) {
                if (*p == '"')
                    q = !q;
                else if (*p == ';' && !q)
                    break;
                rawline[n++] = *p;
            }
            rawline[n] = 0;
            p = rawline;
            while (drvmatch_space(*p))
                p++;
            if (*p == '[') {
                stop = strchr(p, ']');
                drvmatch_trim_copy(p + 1, stop ? stop : p + strlen(p), name,
                                   sizeof(name));
                in = pass == 0
                   ? (strcmp(name, "SOURCEDISKSNAMES") == 0 ||
                      strcmp(name, "SOURCEDISKSNAMES.X86") == 0)
                   : (strcmp(name, "SOURCEDISKSFILES") == 0 ||
                      strcmp(name, "SOURCEDISKSFILES.X86") == 0);
                continue;
            }
            if (!in || !*p)
                continue;
            {
                /* "LEFT = f1, f2, f3, f4": split into up to 5 fields */
                char        left[128], f[5][128];
                const char *eq = strchr(p, '='), *a;
                int         nf = 0, i;

                if (!eq)
                    continue;
                drvmatch_trim_copy(p, eq, left, sizeof(left));
                for (i = 0; i < 5; i++)
                    f[i][0] = 0;
                for (a = eq + 1; nf < 5; nf++) {
                    const char *c = strchr(a, ',');
                    drvmatch_trim_copy(a, c ? c : a + strlen(a), f[nf], sizeof(f[nf]));
                    if (!c)
                        break;
                    a = c + 1;
                }
                if (pass == 0) {
                    /* id = desc, tag, unused, path  -> x86 section overrides */
                    int k;
                    for (k = 0; k < ndisk; k++)
                        if (strcmp(diskid[k], left) == 0)
                            break;
                    if (k == ndisk) {
                        if (ndisk >= DRVMATCH_MAX_DISKS)
                            continue;
                        ndisk++;
                    }
                    strncpy(diskid[k], left, sizeof(diskid[k]) - 1);
                    diskid[k][sizeof(diskid[k]) - 1] = 0;
                    strncpy(diskpath[k], f[3], sizeof(diskpath[k]) - 1);
                    diskpath[k][sizeof(diskpath[k]) - 1] = 0;
                    /* field 2 is a tag file, or a CABINET the files live in */
                    diskcab[k][0] = 0;
                    cab_reported[k] = 0;
                    {
                        size_t tl = strlen(f[1]);
                        if (tl > 4 && strcmp(f[1] + tl - 4, ".CAB") == 0) {
                            strncpy(diskcab[k], f[1], sizeof(diskcab[k]) - 1);
                            diskcab[k][sizeof(diskcab[k]) - 1] = 0;
                        }
                    }
                } else {
                    /* file = diskid [, subdir [, size]] */
                    char        rel[400];
                    const char *dp = "";
                    int         k;
                    size_t      r = 0;

                    int         disk = -1;
                    for (k = 0; k < ndisk; k++)
                        if (strcmp(diskid[k], f[0]) == 0) {
                            dp = diskpath[k];
                            disk = k;
                        }
                    /* Files inside a cabinet: the payload is the CABINET
                     * (Matrox's H010\GSeries.cab holds g400dhm.sys & co). */
                    if (disk >= 0 && diskcab[disk][0]) {
                        if (cab_reported[disk])
                            continue;
                        cab_reported[disk] = 1;
                        f[1][0] = 0;
                        strncpy(left, diskcab[disk], sizeof(left) - 1);
                        left[sizeof(left) - 1] = 0;
                    }
                    rel[0] = 0;
                    for (i = 0; i < 3; i++) {
                        const char *part = i == 0 ? dp : i == 1 ? f[1] : left;
                        while (*part == '\\' || *part == '.') {
                            if (part[0] == '.' && part[1] != '\\' && part[1] != 0)
                                break;          /* "..\x" or ".name": keep */
                            part++;
                        }
                        if (!*part)
                            continue;
                        if (r && r < sizeof(rel) - 1)
                            rel[r++] = '\\';
                        while (*part && r < sizeof(rel) - 1)
                            rel[r++] = *part++;
                        while (r && rel[r - 1] == '\\')
                            r--;
                        rel[r] = 0;
                    }
                    if (rel[0]) {
                        cb(rel, ctx);
                        nfiles++;
                    }
                }
            }
        }
    }
    return nfiles;
}

/* Is an INF's payload present enough to try it? Counts come from walking
 * drvmatch_payload()'s list: files listed / missing, and of those the .SYS
 * drivers listed / missing (a missing file may also be present compressed,
 * NAME.SY_ - the caller checks both before calling it missing).
 *
 * Deliberately COARSE. Many good INFs list files the 32-bit XP install never
 * copies - RTNIC64.SYS, a renamed copy of the INF itself, another OS's .CAT - so
 * "every listed file present" refuses L056\FETNDIS.inf, the right VIA Rhine
 * driver. What must be refused is the shape that really ships here: a payload
 * directory that was never copied, so EVERY driver is absent (all 33 files of
 * G001\CX137529.inf, ATI's B136646\*). Refuse only when EVERY listed .SYS is
 * missing (a file inside a staged .CAB counts as present - the cabinet is what
 * drvmatch_payload reports). An INF that lists no .SYS is never refused: the
 * never-copied-directory shape always lists one, while mesrl.inf (a COM port on
 * in-box serial.sys) or a monitor INF listing only .ICM files would be refused
 * for a file it never copies.
 *
 * A partially-missing payload is caught at install time instead: installs run
 * in SetupAPI's non-interactive mode, so a "Files Needed" prompt FAILS the
 * install rather than blocking the agent on a console nobody is watching. */
static int drvmatch_payload_ok(int nfiles, int nmissing, int nsys, int nsysmissing)
{
    (void)nmissing;
    if (nfiles <= 0 || nsys <= 0)
        return 1;   /* nothing, or no driver, declared: not the shape we refuse */
    return nsysmissing < nsys;
}

/* ---- keep C:\D, or reclaim it? ------------------------------------------ */

/* What the install pass concluded about one unconfigured device. */
enum {
    DRVMATCH_V_UNSEEN = -1,     /* the install pass did not look at it */
    DRVMATCH_V_NONE = 0,        /* nothing in C:\D serves it */
    DRVMATCH_V_INSTALLED,       /* a staged driver installed (restart or not) */
    DRVMATCH_V_FAILED,          /* every candidate refused, or tried out */
    DRVMATCH_V_PREFER,          /* PREFER.TXT owns it (gates the reclaim itself) */
    DRVMATCH_V_UNSEARCHED,      /* C:\D could not be searched, or an install hung */
    DRVMATCH_V_ERRORED          /* installs could not RUN (an error, not a broken
                                   device) and another boot's attempt remains */
};

/* Can installing a driver clear this CM_PROB_* code? Used after a forced
 * install that needed no restart: a device still carrying one of these has the
 * wrong driver, so the next candidate is tried. Any other code (12 resource
 * conflict, ...) is not the driver's fault, and trying lower-ranked drivers
 * would only leave the device on the worst of them. */
static int drvmatch_problem_driver_fixable(unsigned long problem)
{
    switch (problem) {
    case 1:     /* NOT_CONFIGURED */
    case 10:    /* FAILED_START */
    case 18:    /* REINSTALL */
    case 28:    /* FAILED_INSTALL */
    case 31:    /* FAILED_ADD */
    case 37:    /* FAILED_DRIVER_ENTRY */
    case 39:    /* DRIVER_FAILED_LOAD */
        return 1;
    default:
        return 0;
    }
}

/* Is there anything for a driver to do? A device the user disabled (22) or the
 * hardware reports disabled (29) carries a problem code but no installable
 * need, and counting it would hold C:\D on every boot. */
static int drvmatch_problem_wants_driver(unsigned long problem)
{
    return problem != 22 && problem != 29;
}

/* Does this device justify keeping C:\D?
 *
 *   verdict        - from the install pass, or DRVMATCH_V_UNSEEN
 *   scan_ok        - the guard's own walk of C:\D succeeded
 *   confirmed      - (unseen devices only) a candidate passed the payload check
 *                    and Windows' own check
 *
 * An INSTALLED device does not: setupapi has already copied the driver's files
 * to system32 and its INF to C:\WINDOWS\inf, restart pending or not - the same
 * reason the PREFER.TXT path reclaims after a successful force. Holding 2.4 GB
 * through that boot's GAMESYNC would cost a small disk its games for nothing.
 * A FAILED device does not either: a driver that does not work is not a reason
 * to keep 2.4 GB of drivers. When the answer cannot be determined, KEEP. */
static int drvmatch_keeps_tree(int verdict, int scan_ok, int confirmed)
{
    switch (verdict) {
    case DRVMATCH_V_UNSEARCHED:
    case DRVMATCH_V_ERRORED:        /* bounded by the two-boot attempt cap */
        return 1;
    case DRVMATCH_V_UNSEEN:
        return !scan_ok || confirmed;
    default:
        return 0;
    }
}

/* ---- ranked candidates ------------------------------------------------ */

#define DRVMATCH_MAX_CAND 6

/* Insert (idx, path) into a list kept sorted by id index, then arrival order,
 * holding at most DRVMATCH_MAX_CAND. Returns 1 if kept. `path` is copied. */
typedef struct {
    int  n;
    int  idx[DRVMATCH_MAX_CAND];
    char path[DRVMATCH_MAX_CAND][260];
} drvmatch_cands;

static int drvmatch_cand_add(drvmatch_cands *c, int idx, const char *path)
{
    int at = c->n, i;

    while (at > 0 && c->idx[at - 1] > idx)
        at--;
    if (at >= DRVMATCH_MAX_CAND)
        return 0;
    if (c->n < DRVMATCH_MAX_CAND)
        c->n++;
    for (i = c->n - 1; i > at; i--) {
        c->idx[i] = c->idx[i - 1];
        memcpy(c->path[i], c->path[i - 1], sizeof(c->path[i]));
    }
    c->idx[at] = idx;
    for (i = 0; path[i] && i < (int)sizeof(c->path[at]) - 1; i++)
        c->path[at][i] = path[i];
    c->path[at][i] = 0;
    return 1;
}

#endif /* RETRO_DRVMATCH_H */
