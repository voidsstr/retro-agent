/*
 * launchline.h - may a Win9x LAUNCH skip command.com? (agent 1.90.1)
 *
 * 1.89.1 gave a Win9x LAUNCH child its own console (CREATE_NEW_CONSOLE), so a
 * DOS batch no longer runs inside the agent's console VM and stalls it. But
 * Windows 98 then REFUSES a "command.com /c ..." line longer than COMMAND.COM's
 * 127-character tail - CreateProcess error 31 - where the old inherited-console
 * path accepted it: every LAUNCH over ~130 characters failed on .243
 * (2026-09-28), which is every Quake II benchmark launch. A Win32 program does
 * not need a shell at all, and CreateProcess has no such limit for one, so a
 * command whose first token is an .exe is started directly. Batches, .com
 * files and shell syntax keep command.com.
 *
 * Win32-free: tests/native/test_launchline.c compiles it.
 */
#ifndef RETRO_LAUNCHLINE_H
#define RETRO_LAUNCHLINE_H

#include <string.h>

#if defined(__GNUC__)
#define LAUNCHLINE_API static __attribute__((unused))
#else
#define LAUNCHLINE_API static
#endif

/* The first token of args (quotes honoured) into exe; 1 when it names an
 * .exe and the rest of the line has no shell syntax (& | < >) that only
 * command.com would interpret. */
LAUNCHLINE_API int launchline_direct_exe(const char *args, char *exe, size_t cap)
{
    const char *p = args, *start, *end;
    size_t n;
    if (!args || !cap)
        return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        start = ++p;
        while (*p && *p != '"') p++;
        if (*p != '"') return 0;
        end = p++;
    } else {
        start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        end = p;
    }
    n = (size_t)(end - start);
    if (n < 5 || n + 1 > cap)
        return 0;
    if (!(start[n - 4] == '.' && (start[n - 3] | 0x20) == 'e' && (start[n - 2] | 0x20) == 'x'
          && (start[n - 1] | 0x20) == 'e'))
        return 0;
    if (strpbrk(p, "&|<>"))
        return 0;
    memcpy(exe, start, n);
    exe[n] = 0;
    return 1;
}

#endif
