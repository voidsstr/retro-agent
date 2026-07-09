/*
 * rotate_wall.exe - cycle the desktop wallpaper through C:\retro-wall\wall00.bmp
 * .. wallNN.bmp on an interval. Runs invisibly (GUI subsystem), one instance
 * only (named mutex). Started at logon via an HKCU Run key and launched live by
 * deploy_rotation.py.
 *
 *   rotate_wall.exe [seconds]     (default 60)
 *
 * Cross-build:  i686-w64-mingw32-gcc -O2 -mwindows -o rotate_wall.exe \
 *                   rotate_wall.c -luser32
 */
#include <windows.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int interval = 60000;
    if (argc > 1) {
        int v = atoi(argv[1]);
        if (v >= 5) interval = v * 1000;
    }

    /* single instance */
    CreateMutexA(NULL, FALSE, "RetroWallRotateMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    char files[100][MAX_PATH];
    int n = 0;
    for (int i = 0; i < 100; i++) {
        char p[MAX_PATH];
        wsprintfA(p, "C:\\retro-wall\\wall%02d.bmp", i);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES)
            lstrcpyA(files[n++], p);
    }
    if (n == 0) return 1;

    int idx = 0;
    for (;;) {
        SystemParametersInfoA(SPI_SETDESKWALLPAPER, 0, files[idx],
                              SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE);
        idx = (idx + 1) % n;
        Sleep(interval);
    }
    return 0;
}
