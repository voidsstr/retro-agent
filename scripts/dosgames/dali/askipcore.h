/*
 * askipcore.h - the parsing half of ASKIP.COM, Win32/DOS-free so the native
 * regression test (tests/native/test_askip.c) compiles the code DOS runs.
 */
#ifndef ASKIPCORE_H
#define ASKIPCORE_H
#include <stdio.h>
#include <string.h>

/* 1 if s is a dotted quad with four parts 0..255 and nothing else */
static int ipok(const char *s)
{
    int parts = 0, digits = 0;
    unsigned v = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s - '0');
            if (++digits > 3 || v > 255)
                return 0;
        } else if (*s == '.' || *s == 0) {
            if (!digits)
                return 0;
            parts++;
            digits = 0;
            v = 0;
            if (*s == 0)
                break;
            if (parts > 3)
                return 0;
        } else {
            return 0;
        }
    }
    return parts == 4;
}

/* first token of the first non-comment line of f, into out (cap 15 chars) */
static void askip_read_default(const char *f, char *out)
{
    FILE *fp = fopen(f, "r");
    char line[128];
    out[0] = 0;
    if (!fp)
        return;
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        int n = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == ';' || *p == '#' || *p == '\r' || *p == '\n' || *p == 0)
            continue;
        while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\r' && p[n] != '\n' && n < 15)
            n++;
        memcpy(out, p, n);
        out[n] = 0;
        break;
    }
    fclose(fp);
    if (!ipok(out))
        out[0] = 0;
}


/* 1 if the answer means "cancel": Q or X on its own. DOS buffered input
 * treats ESC as "clear the line", so ESC can never be the cancel key. */
static int askip_is_cancel(const char *s)
{
    return (s[0] == 'q' || s[0] == 'Q' || s[0] == 'x' || s[0] == 'X') && s[1] == 0;
}

#endif
