/*
 * arrange_icons.exe - move all Windows desktop icons into a compact grid in the
 * bottom-right corner (matching the blank "icon well" the dossier wallpaper
 * reserves there, ICON_WELL_FRAC in gen_wallpaper.py).
 *
 * The desktop icons live in a SysListView32 hosted by Progman/WorkerW. We turn
 * off auto-arrange (so free positions stick) and LVM_SETITEMPOSITION each item.
 * LVM_GETITEMCOUNT / LVM_SETITEMPOSITION pass values (not pointers) so they work
 * cross-process without injecting into explorer.
 *
 * Cross-build:  i686-w64-mingw32-gcc -O2 -o arrange_icons.exe arrange_icons.c \
 *                   -luser32 -lgdi32
 */
#include <windows.h>
#include <stdio.h>

#ifndef LVM_FIRST
#define LVM_FIRST 0x1000
#endif
#define LVM_GETITEMCOUNT   (LVM_FIRST + 4)
#define LVM_SETITEMPOSITION (LVM_FIRST + 15)
#ifndef LVS_AUTOARRANGE
#define LVS_AUTOARRANGE 0x0100
#endif

static HWND find_desktop_listview(void) {
    HWND prog = FindWindowA("Progman", NULL);
    HWND defview = FindWindowExA(prog, NULL, "SHELLDLL_DefView", NULL);
    if (!defview) {
        HWND worker = NULL;
        while ((worker = FindWindowExA(NULL, worker, "WorkerW", NULL)) != NULL) {
            defview = FindWindowExA(worker, NULL, "SHELLDLL_DefView", NULL);
            if (defview) break;
        }
    }
    if (!defview) return NULL;
    return FindWindowExA(defview, NULL, "SysListView32", NULL);
}

int main(void) {
    HWND lv = find_desktop_listview();
    if (!lv) { printf("desktop listview not found\n"); return 1; }

    /* free placement: drop auto-arrange so our positions stick */
    LONG style = GetWindowLongA(lv, GWL_STYLE);
    SetWindowLongA(lv, GWL_STYLE, style & ~LVS_AUTOARRANGE);

    int n = (int)SendMessageA(lv, LVM_GETITEMCOUNT, 0, 0);
    if (n <= 0) { printf("no desktop icons\n"); return 0; }

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    /* Packed spacing so even 30+ icons fit the well while staying readable.
       Must match the well the wallpaper reserves (ICON_WELL_FRAC=0.36, and the
       right column spans the whole game+events band ~ bottom 66% of height). */
    int sx = 70, sy = 72;

    int rightMargin = 16, bottomMargin = 44;
    int wellW = (int)(scrW * 0.36);
    int maxRows = (int)((scrH * 0.66) / sy);
    if (maxRows < 1) maxRows = 1;

    int cols = wellW / sx;
    if (cols < 1) cols = 1;
    /* widen (spill left) only as a last resort if they can't fit the band */
    while ((n + cols - 1) / cols > maxRows) cols++;
    int rows = (n + cols - 1) / cols;

    int startX = scrW - rightMargin - cols * sx;
    int startY = scrH - bottomMargin - rows * sy;
    if (startX < 0) startX = 0;
    if (startY < 0) startY = 0;

    for (int i = 0; i < n; i++) {
        int c = i % cols, r = i / cols;
        int x = startX + c * sx;
        int y = startY + r * sy;
        SendMessageA(lv, LVM_SETITEMPOSITION, i, MAKELPARAM(x, y));
    }
    printf("moved %d icons to bottom-right well (%d cols x %d rows)\n", n, cols, rows);
    return 0;
}
