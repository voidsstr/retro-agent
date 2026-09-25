/*
 * httpdate.h - find and parse the Date: header of an HTTP response.
 *
 * Win32-free so tests/native/test_httpdate.c compiles the code the agent runs.
 * Used by agent/src/clockfix.c, which sets a box's clock from the NAS when the
 * clock is years wrong (a dead CMOS battery - .243 booted into 1980 every
 * time it was powered on).
 *
 * Only the RFC 1123 form is accepted ("Fri, 25 Sep 2026 03:54:00 GMT"), which
 * is what HTTP/1.1 servers must send. Anything else is refused rather than
 * guessed at: a clock set from a misparsed date is worse than a clock left
 * alone, because nothing afterwards says it was wrong.
 */
#ifndef HTTPDATE_H
#define HTTPDATE_H

#include <string.h>

#ifndef HD_FN
#define HD_FN static
#endif

typedef struct {
    int year, month, day;        /* month 1-12 */
    int hour, minute, second;
} hd_time_t;

static int hd__lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int hd__num(const char **pp, int digits, int *out)
{
    const char *p = *pp;
    int v = 0, i;
    for (i = 0; i < digits; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10 + (p[i] - '0');
    }
    *out = v;
    *pp = p + digits;
    return 1;
}

static int hd__lit(const char **pp, char c)
{
    if (**pp != c) return 0;
    (*pp)++;
    return 1;
}

/* "Fri, 25 Sep 2026 03:54:00 GMT" -> 1 and *t filled, or 0. */
HD_FN int hd_parse_rfc1123(const char *s, hd_time_t *t)
{
    static const char mon[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    const char *p = s;
    int i, m = 0;
    while (*p == ' ' || *p == '\t') p++;
    /* day name: three letters then a comma - not checked against the date */
    for (i = 0; i < 3; i++)
        if (!((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z'))) return 0;
    p += 3;
    if (!hd__lit(&p, ',') || !hd__lit(&p, ' ')) return 0;
    if (!hd__num(&p, 2, &t->day) || !hd__lit(&p, ' ')) return 0;
    for (i = 0; i < 12; i++)
        if (hd__lower(p[0]) == mon[i * 3] && hd__lower(p[1]) == mon[i * 3 + 1]
                && hd__lower(p[2]) == mon[i * 3 + 2]) { m = i + 1; break; }
    if (!m) return 0;
    t->month = m;
    p += 3;
    if (!hd__lit(&p, ' ') || !hd__num(&p, 4, &t->year) || !hd__lit(&p, ' ')) return 0;
    if (!hd__num(&p, 2, &t->hour) || !hd__lit(&p, ':')
            || !hd__num(&p, 2, &t->minute) || !hd__lit(&p, ':')
            || !hd__num(&p, 2, &t->second)) return 0;
    if (!hd__lit(&p, ' ') || hd__lower(p[0]) != 'g' || hd__lower(p[1]) != 'm'
            || hd__lower(p[2]) != 't') return 0;
    if (t->day < 1 || t->day > 31 || t->hour > 23 || t->minute > 59 || t->second > 60)
        return 0;
    return 1;
}

/* Find the Date header in a raw response (headers only need be present) and
 * parse it. Case-insensitive header name, as HTTP requires. */
HD_FN int hd_find_date(const char *resp, hd_time_t *t)
{
    const char *p = resp;
    while (p && *p) {
        const char *line = p;
        if (hd__lower(line[0]) == 'd' && hd__lower(line[1]) == 'a'
                && hd__lower(line[2]) == 't' && hd__lower(line[3]) == 'e'
                && line[4] == ':')
            return hd_parse_rfc1123(line + 5, t);
        p = strchr(line, '\n');
        if (p) p++;
        if (p && (*p == '\r' || *p == '\n')) break;   /* end of headers */
    }
    return 0;
}

#endif /* HTTPDATE_H */
