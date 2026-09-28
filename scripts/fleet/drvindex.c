/*
 * drvindex.c - index a driver store with the AGENT'S OWN matcher (1.89.0).
 *
 * The agent's install pass decides "does this INF serve this device?" with
 * agent/shared/drvmatch.h (model lines only, XP decorations only, [Strings]
 * expansion) and refuses 3dfx with agent/shared/drvsafe.h. An index a box can
 * look up without scanning 3,700 INFs over SMB has to give the SAME answers,
 * so this tool compiles those two headers and runs them over every INF: no
 * second parser to drift from the first.
 *
 *   drvindex <store-root> [<dir>...]      (dirs default to every subdirectory)
 *
 * Output on stdout, one line per (id, INF), ids upper-case:
 *   <BUCKET>\t<ID>\t<DIR>\\<INF>\t<DriverVer or ->   (bucket: agent/shared/drvstore.h)
 * plus "#skip3dfx\t<DIR>\\<INF>\t<word>" for each 3dfx INF left out.
 * The Python wrapper (scripts/fleet/driverstore.py) buckets and publishes it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

#define GS_INF_READ_MAX (2 * 1024 * 1024)
#include "../../agent/shared/drvmatch.h"
#include "../../agent/shared/drvsafe.h"
#include "../../agent/shared/drvstore.h"

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int ends_inf(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && (n[l - 4] == '.') && toupper((unsigned char)n[l - 3]) == 'I'
        && toupper((unsigned char)n[l - 2]) == 'N' && toupper((unsigned char)n[l - 1]) == 'F';
}

/* DriverVer=MM/DD/YYYY[,a.b.c.d] from the [Version] section only (not a
 * "DriverVersion" string, not a comment), before prepare blanks it. */
static void driver_ver(const char *buf, char *out, size_t cap)
{
    const char *line, *next;
    int in_ver = 0;
    out[0] = '-'; out[1] = 0;
    for (line = buf; *line; line = next) {
        const char *p = line;
        size_t k = 0;
        next = strchr(line, '\n');
        next = next ? next + 1 : line + strlen(line);
        while (p < next && (*p == ' ' || *p == '\t')) p++;
        if (*p == '[') { in_ver = strncmp(p, "[VERSION]", 9) == 0; continue; }
        if (!in_ver || strncmp(p, "DRIVERVER", 9) != 0) continue;
        p += 9;
        while (p < next && (*p == ' ' || *p == '\t')) p++;
        if (*p != '=') continue;
        p++;
        while (p < next && *p != '\r' && *p != '\n' && *p != ';' && k + 1 < cap) {
            if (*p != ' ' && *p != '\t' && *p != '"') out[k++] = *p;
            p++;
        }
        out[k] = 0;
        if (!k) { out[0] = '-'; out[1] = 0; }
        return;
    }
}

/* What separates the id fields drvmatch_prepare leaves on a model line
 * ("   id1, id2") and the [Strings] values it keeps for %KEY% ids. Everything
 * else is part of an id - including '{' '}': "{1A3E09BE-...}\NVNET_DEV0057" is
 * ONE id, and splitting at the brace (drvmatch_idch's set) indexed it as
 * "\NVNET_DEV0057", which no device carries. */
static int id_sep(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == '"' || c == '=';
}

static void index_inf(const char *root, const char *dir, const char *name, char *buf)
{
    char   path[4096], ver[80], tok[512];
    FILE  *f;
    size_t got, len, k;
    char  *p;
    const char *hit;

    snprintf(path, sizeof(path), "%s/%s/%s", root, dir, name);
    f = fopen(path, "rb");
    if (!f) return;
    got = fread(buf, 1, GS_INF_READ_MAX, f);
    fclose(f);
    if (!got) return;
    buf[got] = buf[got + 1] = 0;
    len = drvmatch_fold_utf16(buf, got);
    buf[len] = 0;
    for (p = buf; *p; p++) *p = (char)toupper((unsigned char)*p);
    if ((hit = drvsafe_inf_text_hit(buf)) != NULL) {
        printf("#skip3dfx\t%s\\%s\t%s\n", dir, name, hit);
        return;
    }
    driver_ver(buf, ver, sizeof(ver));
    drvmatch_prepare(buf);
    for (p = buf; *p; ) {
        if (id_sep(*p)) { p++; continue; }
        for (k = 0; *p && !id_sep(*p); p++)
            if (k + 1 < sizeof(tok)) tok[k++] = *p;
        tok[k] = 0;
        /* a hardware id has a bus prefix or is a *PNP / EISA style id */
        if (strchr(tok, '\\') || tok[0] == '*') {
            char bucket[16];
            drvstore_bucket(tok, bucket);
            printf("%s\t%s\t%s\\%s\t%s\n", bucket, tok, dir, name, ver);
        }
    }
}

int main(int argc, char **argv)
{
    char *buf;
    int   i;

    if (argc < 2) {
        fprintf(stderr, "usage: drvindex <store-root> [<dir>...]\n");
        return 2;
    }
    buf = malloc(GS_INF_READ_MAX + 2);
    if (!buf) return 1;
    if (argc > 2) {
        for (i = 2; i < argc; i++) {
            char d[4096];
            DIR *dh;
            struct dirent *e;
            snprintf(d, sizeof(d), "%s/%s", argv[1], argv[i]);
            if (!(dh = opendir(d))) continue;
            while ((e = readdir(dh)) != NULL)
                if (ends_inf(e->d_name)) index_inf(argv[1], argv[i], e->d_name, buf);
            closedir(dh);
        }
    } else {
        DIR *top = opendir(argv[1]);
        struct dirent *de;
        if (!top) { perror(argv[1]); return 1; }
        while ((de = readdir(top)) != NULL) {
            char d[4096];
            DIR *dh;
            struct dirent *e;
            if (de->d_name[0] == '.') continue;
            snprintf(d, sizeof(d), "%s/%s", argv[1], de->d_name);
            if (!is_dir(d) || !(dh = opendir(d))) continue;
            while ((e = readdir(dh)) != NULL)
                if (ends_inf(e->d_name)) index_inf(argv[1], de->d_name, e->d_name, buf);
            closedir(dh);
        }
        closedir(top);
    }
    free(buf);
    return 0;
}
