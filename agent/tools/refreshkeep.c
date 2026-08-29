/*
 * refreshkeep - hold a fullscreen game at a chosen refresh rate.
 *
 * A vintage game that changes video mode without asking for a frequency gets
 * 60Hz on XP, whatever the desktop was set to (see refreshlogic.h for the
 * measurement). Quake II, Unreal Tournament 99 and GoldSrc/Half-Life all do
 * this and have no in-engine refresh setting. ForceWare 71.89 on XP has no
 * refresh-rate override page either, so nothing inside the box fixes it.
 *
 * refreshkeep sits beside the game, watches the current mode, and re-applies
 * it WITH DM_DISPLAYFREQUENCY whenever it drifts off target. It exits when the
 * watched process is gone (or after a time limit), so it never lingers.
 *
 *   refreshkeep 100 quake2.exe          -> 100Hz while quake2.exe runs
 *   refreshkeep max UnrealTournament.exe 900
 *   refreshkeep 100 - 60                -> 100Hz for 60 seconds, no process
 *
 * Applies with flags 0 (this session only) - it deliberately does NOT write
 * CDS_UPDATEREGISTRY, so a game's odd resolution can never become the stored
 * desktop mode. setrefresh.exe remains the tool for the persistent desktop.
 *
 * Build (mingw):
 *   i686-w64-mingw32-gcc -O2 -o refreshkeep.exe refreshkeep.c -lgdi32
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "refreshlogic.h"

#define MAX_RATES 64

static int stricmp_ascii(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb)
            return ca - cb;
        a++; b++;
    }
    return (unsigned char) *a - (unsigned char) *b;
}

static int process_running(const char *name)
{
    HANDLE snap;
    PROCESSENTRY32 pe;
    int found = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return -1;                      /* cannot tell */
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (stricmp_ascii(pe.szExeFile, name) == 0) {
                found = 1;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

/* Collect the refresh rates the driver offers for w x h x bpp. */
static int rates_for_mode(DWORD w, DWORD h, DWORD bpp, int *out, int max)
{
    int i, n = 0;
    for (i = 0; n < max; i++) {
        DEVMODE m;
        ZeroMemory(&m, sizeof(m));
        m.dmSize = sizeof(m);
        if (!EnumDisplaySettings(NULL, i, &m))
            break;
        if (m.dmPelsWidth == w && m.dmPelsHeight == h && m.dmBitsPerPel == bpp)
            out[n++] = (int) m.dmDisplayFrequency;
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *proc = NULL;
    int want = 0, limit = 1800, elapsed = 0, seen = 0, forced = 0;

    if (argc < 2) {
        printf("usage: refreshkeep <hz|max> [procname.exe|-] [maxseconds]\n");
        return 1;
    }
    if (stricmp_ascii(argv[1], "max") != 0)
        want = atoi(argv[1]);
    if (argc >= 3 && strcmp(argv[2], "-") != 0)
        proc = argv[2];
    if (argc >= 4)
        limit = atoi(argv[3]);
    if (limit <= 0)
        limit = 1800;

    printf("refreshkeep: target=%s proc=%s limit=%ds\n",
           want ? argv[1] : "max", proc ? proc : "(none)", limit);
    fflush(stdout);

    for (; elapsed < limit; elapsed++, Sleep(1000)) {
        DEVMODE cur;
        int rates[MAX_RATES], n, target;

        if (proc) {
            int live = process_running(proc);
            if (live > 0)
                seen = 1;
            /* Give the game 60s to appear, then exit once it is gone. */
            else if (live == 0 && (seen || elapsed > 60))
                break;
        }

        ZeroMemory(&cur, sizeof(cur));
        cur.dmSize = sizeof(cur);
        if (!EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &cur))
            continue;

        n = rates_for_mode(cur.dmPelsWidth, cur.dmPelsHeight,
                           cur.dmBitsPerPel, rates, MAX_RATES);
        target = rk_pick_refresh(rates, n, want);
        if (!rk_should_force((int) cur.dmDisplayFrequency, target))
            continue;

        {
            DEVMODE set;
            LONG r;
            ZeroMemory(&set, sizeof(set));
            set.dmSize = sizeof(set);
            set.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                           DM_DISPLAYFREQUENCY;
            set.dmPelsWidth = cur.dmPelsWidth;
            set.dmPelsHeight = cur.dmPelsHeight;
            set.dmBitsPerPel = cur.dmBitsPerPel;
            set.dmDisplayFrequency = (DWORD) target;
            r = ChangeDisplaySettings(&set, 0);
            printf("t=%ds %lux%lux%lu %luHz -> %dHz: rc=%ld\n", elapsed,
                   cur.dmPelsWidth, cur.dmPelsHeight, cur.dmBitsPerPel,
                   cur.dmDisplayFrequency, target, r);
            fflush(stdout);
            if (r == DISP_CHANGE_SUCCESSFUL)
                forced++;
        }
    }

    printf("refreshkeep: done after %ds, %d mode change(s)\n", elapsed, forced);
    return 0;
}
