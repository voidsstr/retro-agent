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
/* shell view context-menu command IDs (FCIDM_SHVIEW_*) */
#define FCIDM_SHVIEW_AUTOARRANGE 0x7031
#define FCIDM_SHVIEW_SNAPTOGRID  0x7032

static HWND g_defview;

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
    g_defview = defview;
    if (!defview) return NULL;
    return FindWindowExA(defview, NULL, "SysListView32", NULL);
}

int main(int argc, char **argv) {
    HWND lv = find_desktop_listview();
    if (!lv) { printf("desktop listview not found\n"); return 1; }

    /* Optional: turn OFF "Align to Grid" (snap-to-grid). With it on, a displaced
       icon re-snaps to a grid cell - and an immovable item (a folder whose saved
       position won't persist) can bump another icon to a top cell on every
       refresh. It's a toggle, so only pass "nogrid" when it's currently on. */
    if (argc > 1 && strcmp(argv[1], "nogrid") == 0 && g_defview) {
        /* PostMessage (not Send) - a synchronous send to the shell can block */
        PostMessageA(g_defview, WM_COMMAND, FCIDM_SHVIEW_SNAPTOGRID, 0);
        Sleep(500);
    }

    /* "Auto Arrange" must be OFF or the shell re-snaps icons to the top-left
       grid (and every wallpaper-rotation refresh re-snaps them). Toggle it off
       via the shell view's own menu command so the change persists, not just
       the window style. It's a toggle, so only send it when currently on. */
    LONG style = GetWindowLongA(lv, GWL_STYLE);
    if ((style & LVS_AUTOARRANGE) && g_defview) {
        SendMessageA(g_defview, WM_COMMAND, FCIDM_SHVIEW_AUTOARRANGE, 0);
        Sleep(200);
        style = GetWindowLongA(lv, GWL_STYLE);
    }
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

    /* LVM_GETITEMCOUNT occasionally under-reports by one (a freshly created
       folder can escape the count and sit wherever it was, e.g. overlapping the
       header). Position a few indices past the reported count too - out-of-range
       indices just no-op, but a real stray gets pulled into the well. */
    int limit = n + 8;
    for (int i = 0; i < limit; i++) {
        int c = i % cols, r = i / cols;
        int x = startX + c * sx;
        int y = startY + r * sy;
        SendMessageA(lv, LVM_SETITEMPOSITION, i, MAKELPARAM(x, y));
    }
    printf("moved %d icons to bottom-right well (%d cols x %d rows)\n", n, cols, rows);
    return 0;
}
