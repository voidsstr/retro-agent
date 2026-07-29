/*
 * setrefresh - set the Windows desktop to the HIGHEST monitor-safe refresh
 *              rate for a given (or the current) resolution, and PERSIST it
 *              across restarts (ChangeDisplaySettings CDS_UPDATEREGISTRY).
 *
 * Enumerates the modes the display driver actually offers (EnumDisplaySettings
 * -> EDID-safe: the driver won't advertise a mode the monitor can't do), picks
 * the highest refresh at the target resolution, applies it now AND writes it to
 * the registry so XP uses it on the next boot. On a fleet Voodoo3 (.124) the
 * monitor tops out at 100Hz @ 1024x768.
 *
 *   setrefresh                 -> max refresh at the CURRENT desktop mode
 *   setrefresh <w> <h> [bpp]   -> max refresh at that resolution
 *
 * Prints the available refresh list + the chosen rate + the set result.
 * Returns 0 on DISP_CHANGE_SUCCESSFUL.
 *
 * NOTE on restarts: CDS_UPDATEREGISTRY survives a *graceful* reboot, but a hard
 * power-cycle can come back at 60Hz. Belt-and-suspenders is a delayed logon
 * re-apply (setrefresh_boot.vbs -> setrefresh_boot.bat waits for the display
 * driver to settle, then runs this twice), wired via HKLM..\Run.
 *
 * Build (mingw):
 *   i686-w64-mingw32-gcc -O2 -o setrefresh.exe setrefresh.c -lgdi32
 */
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    DEVMODE dm;
    int i, bestHz = 0;
    DWORD curW, curH, curBpp;

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        printf("ENUM_CURRENT failed\n");
        return 2;
    }
    curW = dm.dmPelsWidth;
    curH = dm.dmPelsHeight;
    curBpp = dm.dmBitsPerPel;
    printf("current: %lux%lux%lu @ %luHz\n",
           curW, curH, curBpp, dm.dmDisplayFrequency);

    if (argc >= 3) {
        curW = (DWORD) atoi(argv[1]);
        curH = (DWORD) atoi(argv[2]);
    }
    if (argc >= 4)
        curBpp = (DWORD) atoi(argv[3]);

    printf("available refresh at %lux%lux%lu:", curW, curH, curBpp);
    for (i = 0;; i++) {
        DEVMODE m;
        ZeroMemory(&m, sizeof(m));
        m.dmSize = sizeof(m);
        if (!EnumDisplaySettings(NULL, i, &m))
            break;
        if (m.dmPelsWidth == curW && m.dmPelsHeight == curH &&
            m.dmBitsPerPel == curBpp) {
            printf(" %lu", m.dmDisplayFrequency);
            /* < 200 guards against the bogus 1Hz/"optimal" sentinel modes */
            if ((int) m.dmDisplayFrequency > bestHz &&
                m.dmDisplayFrequency < 200)
                bestHz = (int) m.dmDisplayFrequency;
        }
    }
    printf("\nbest = %dHz\n", bestHz);
    if (bestHz <= 0) {
        printf("no modes found\n");
        return 3;
    }

    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                  DM_DISPLAYFREQUENCY;
    dm.dmPelsWidth = curW;
    dm.dmPelsHeight = curH;
    dm.dmBitsPerPel = curBpp;
    dm.dmDisplayFrequency = (DWORD) bestHz;
    {
        LONG r1 = ChangeDisplaySettings(&dm, CDS_UPDATEREGISTRY); /* persist */
        LONG r2 = ChangeDisplaySettings(&dm, 0);                  /* apply now */
        printf("set %lux%lux%lu @ %dHz: updatereg=%ld apply=%ld "
               "(0=SUCCESSFUL)\n", curW, curH, curBpp, bestHz, r1, r2);
        return (r1 == DISP_CHANGE_SUCCESSFUL) ? 0 : 4;
    }
}
