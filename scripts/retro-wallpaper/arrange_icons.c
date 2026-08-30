/*
 * arrange_icons.exe - park the Windows desktop icons in the wallpaper's ICON
 * BAY, which the current fleet wallpaper draws TOP-LEFT.
 *
 * HISTORY, because getting this wrong has cost us twice. The first version of
 * this program packed icons into a well in the BOTTOM-RIGHT, matching the old
 * gen_wallpaper.py ICON_WELL_FRAC art. The wallpaper was then redesigned
 * (scripts/retro-wallpaper/gen_retro_wall.py:icon_bay) to draw one visible cell
 * per icon TOP-LEFT, and agent/src/gamesync.c:gs_icon_bay() was written to
 * match it - but THIS program was never updated. Since agent/src/retrowall.c
 * runs it on EVERY agent start, every restart and every reboot dragged the
 * icons back to the bottom-right, straight off the drawn bay, undoing what
 * GAMESYNC had just done. Measured on 192.168.1.145 on 2026-08-29: the staged
 * binary was byte-identical (md5 d80f8372...) to the old build and its own
 * message string still read "moved %d icons to bottom-right well".
 *
 * THE GEOMETRY BELOW MUST STAY IDENTICAL TO BOTH
 *   scripts/retro-wallpaper/gen_retro_wall.py : icon_bay()
 *   agent/src/gamesync.c                      : gs_icon_bay()
 * (tests/native/test_icon_bay.c pins the last two together.)
 *
 * ONE DELIBERATE DIFFERENCE from gs_icon_bay(), and it is a fix rather than a
 * drift: WHAT TO DO WHEN THERE ARE MORE ICONS THAN BAY SLOTS. The agent keeps
 * packing DOWNWARD, which at 1024x768 (bay 4 cols x 8 rows = 32 slots) with the
 * fleet's 65 shortcuts puts roughly half of them BELOW THE BOTTOM OF THE
 * SCREEN, where the desktop has no scrollbar and they cannot be reached at all.
 * This program instead widens into extra COLUMNS to the right - outside the
 * drawn panel, which is visibly imperfect, but every icon stays on screen and
 * clickable. An icon you can see in the wrong place beats one you cannot reach.
 *
 * Cross-build:  i686-w64-mingw32-gcc -O2 -o arrange_icons.exe arrange_icons.c \
 *                   -luser32 -lgdi32
 */
#include <windows.h>
#include <stdio.h>

#ifndef LVM_FIRST
#define LVM_FIRST 0x1000
#endif
#define LVM_GETITEMCOUNT    (LVM_FIRST + 4)
#define LVM_SETITEMPOSITION (LVM_FIRST + 15)
#ifndef LVS_AUTOARRANGE
#define LVS_AUTOARRANGE 0x0100
#endif
#define LVM_GETEXSTYLE      (LVM_FIRST + 55)
#define LVM_SETEXSTYLE      (LVM_FIRST + 54)
#ifndef LVS_EX_SNAPTOGRID
#define LVS_EX_SNAPTOGRID 0x00080000
#endif
/* shell view context-menu command IDs (FCIDM_SHVIEW_*) */
#define FCIDM_SHVIEW_AUTOARRANGE 0x7031

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

int main(void) {
    HWND lv = find_desktop_listview();
    int i, n, pass, cols, rows, eff_cols, max_cols, max_rows;
    int scrW, scrH, margin_x, margin_y, bay_x, bay_y;
    const int cell_w = 76, cell_h = 80, header_h = 34;
    LONG style;

    if (!lv) { printf("desktop listview not found\n"); return 1; }

    /* THE OLD "nogrid" OPTION IS GONE ON PURPOSE - DO NOT PUT IT BACK.
       It posted FCIDM_SHVIEW_SNAPTOGRID (0x7032) to the shell view meaning
       "toggle Align to Grid". On Windows XP that command id is NOT align-to-
       grid: sending it on 192.168.1.145 (2026-08-29) raised a modal
       "Desktop - This folder cannot be customized. It is either marked as
       read-only, or your system administrator has disabled this
       functionality." - i.e. it reached Customize This Folder instead. A stray
       modal on this fleet blocks every later `start`, so a cosmetic option
       that can leave one behind is not worth having. It was also unnecessary:
       with auto-arrange off, the placement below lands cleanly on its own. */

    /* "Auto Arrange" must be OFF or the shell re-snaps everything to its own
       top-left grid and our positions never stick. FCIDM_SHVIEW_AUTOARRANGE is
       a toggle, so only send it when the style says it is currently on;
       sending it blindly turns it ON. Clear the style bit as well - the menu
       command alone has been seen to fail on XP. */
    style = GetWindowLongA(lv, GWL_STYLE);
    if ((style & LVS_AUTOARRANGE) && g_defview) {
        SendMessageA(g_defview, WM_COMMAND, FCIDM_SHVIEW_AUTOARRANGE, 0);
        Sleep(200);
        style = GetWindowLongA(lv, GWL_STYLE);
    }
    SetWindowLongA(lv, GWL_STYLE, style & ~LVS_AUTOARRANGE);

    /* "Align icons to grid" is a SECOND, INDEPENDENT setting and clearing
       auto-arrange does nothing about it - Windows enables it by default, so
       it affects every box. While LVS_EX_SNAPTOGRID is set the shell ROUNDS
       every position we ask for to its own grid, whose row pitch is the icon
       spacing plus the label (measured at 103 px on 1920x1080), so a bay drawn
       with 80 px cells gets icons 103 px apart and they walk out of their
       slots. Same finding and same remedy as agent/src/gamesync.c's
       gs_arrange_icons(); keep the two in step. Unlike auto-arrange this is
       not a toggle - the message takes a (mask, value) pair, so value 0 clears
       it deterministically and can never switch it on. */
    {
        DWORD exst = (DWORD)SendMessageA(lv, LVM_GETEXSTYLE, 0, 0);
        if (exst & LVS_EX_SNAPTOGRID)
            SendMessageA(lv, LVM_SETEXSTYLE, LVS_EX_SNAPTOGRID, 0);
    }

    n = (int)SendMessageA(lv, LVM_GETITEMCOUNT, 0, 0);
    if (n <= 0) { printf("no desktop icons\n"); return 0; }

    scrW = GetSystemMetrics(SM_CXSCREEN);
    scrH = GetSystemMetrics(SM_CYSCREEN);

    /* ---- mirror of icon_bay() / gs_icon_bay() ---- */
    margin_x = (int)(scrW * 0.018); if (margin_x < 18) margin_x = 18;
    margin_y = (int)(scrH * 0.030); if (margin_y < 18) margin_y = 18;
    cols = (int)((scrW * 0.34) / cell_w); if (cols < 2) cols = 2;
    rows = (scrH - margin_y - header_h - 24) / cell_h; if (rows < 3) rows = 3;
    bay_x = margin_x;
    bay_y = margin_y + header_h;
    /* ---------------------------------------------- */

    /* Widen rather than run off the bottom (see the header comment).
     *
     * Note the row budget is how many rows fit ON THE SCREEN, not how many the
     * drawn bay has: the bay is the art, the screen edge is the hard limit, and
     * spilling one row past the panel is fine while spilling past the screen is
     * not. Where even the full screen width cannot hold them all (800x600 with
     * the whole library, say) we take the widest layout that fits and let the
     * remainder run on - there is nowhere else for it to go, and the first
     * screenful is still the priority order the library asked for. */
    max_rows = (scrH - bay_y - 6) / cell_h;
    if (max_rows < 1) max_rows = 1;
    max_cols = (scrW - bay_x - 8) / cell_w;
    if (max_cols < cols) max_cols = cols;

    eff_cols = cols;
    if (n > cols * max_rows) {
        eff_cols = (n + max_rows - 1) / max_rows;
        if (eff_cols > max_cols) eff_cols = max_cols;
    }

    /* TWO PASSES, and the second one is not superstition. Measured on
       192.168.1.143 (2026-08-29): a single pass immediately after a GAMESYNC
       left a ragged block with holes in it and two icons stranded above the
       bay header, because the shell was still rebuilding the desktop listview
       from the shortcuts the sync had just written and re-laid some of them
       after we had placed them. Running the identical pass again a second
       later produced a perfect 9x8 block. The pass is idempotent and costs
       nothing, so just do it twice.

       LVM_GETITEMCOUNT also occasionally under-reports by one (a freshly
       created folder can escape the count). Positioning a few extra indices is
       a no-op for out-of-range items but pulls a real stray into the bay. */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < n + 8; i++) {
            int c = i % eff_cols, r = i / eff_cols;
            SendMessageA(lv, LVM_SETITEMPOSITION, i,
                         MAKELPARAM(bay_x + c * cell_w + 6,
                                    bay_y + r * cell_h + 6));
        }
        if (pass == 0) {
            Sleep(1200);
            /* The count can grow between passes while the shell is still
               populating; re-read it so late arrivals get a slot too. */
            n = (int)SendMessageA(lv, LVM_GETITEMCOUNT, 0, 0);
            if (n <= 0) break;
        }
    }
    printf("moved %d icons into the top-left icon bay "
           "(%dx%d bay, laid out %d wide, screen %dx%d)\n",
           n, cols, rows, eff_cols, scrW, scrH);
    return 0;
}
