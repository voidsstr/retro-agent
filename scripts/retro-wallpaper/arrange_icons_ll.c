/*
 * arrange_icons_ll.exe - arrange the Windows desktop icons into a compact block
 * in the BOTTOM-LEFT corner, ORGANIZED BY THEME with "My Computer" first.
 *
 * Unlike arrange_icons.exe (which just packs them bottom-right in their existing
 * order), this reads each icon's LABEL cross-process, classifies it into a theme
 * group, sorts (My Computer -> System -> Internet -> Games -> Tools -> Media ->
 * Other, alpha within a group), and lays the sorted list out column-major in a
 * block anchored to the bottom-left so each theme clusters together.
 *
 * Reading SysListView32 item text cross-process needs the text buffer to live in
 * explorer's address space, so we VirtualAllocEx a remote LVITEMW + char buffer,
 * SendMessage LVM_GETITEMTEXTW, then ReadProcessMemory the label back. Positions
 * are set with LVM_SETITEMPOSITION (value args, no pointer) as before.
 *
 * Cross-build:  i686-w64-mingw32-gcc -O2 -o arrange_icons_ll.exe arrange_icons_ll.c \
 *                   -luser32 -lgdi32
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>

#ifndef LVM_FIRST
#define LVM_FIRST 0x1000
#endif
#define LVM_GETITEMCOUNT    (LVM_FIRST + 4)
#define LVM_SETITEMPOSITION (LVM_FIRST + 15)
#define LVM_GETITEMTEXTW    (LVM_FIRST + 115)
#ifndef LVIF_TEXT
#define LVIF_TEXT 0x0001
#endif
#ifndef LVS_AUTOARRANGE
#define LVS_AUTOARRANGE 0x0100
#endif
#define FCIDM_SHVIEW_AUTOARRANGE 0x7031
#define FCIDM_SHVIEW_SNAPTOGRID  0x7032

/* Remote LVITEMW layout must match the target (all these boxes are 32-bit
 * explorer). Use the SDK LVITEMW; we allocate it in the remote process. */

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

/* theme rank: lower = earlier. My Computer is forced to -1 elsewhere. */
enum { T_SYSTEM=0, T_INTERNET, T_GAMES, T_TOOLS, T_MEDIA, T_OTHER, T_NGROUPS };

static int classify(const wchar_t *lw) {
    wchar_t s[260]; int i;
    for (i = 0; lw[i] && i < 259; i++) s[i] = towlower(lw[i]);
    s[i] = 0;
    #define HAS(sub) (wcsstr(s, L##sub) != NULL)
    /* System / shell */
    if (HAS("my documents") || HAS("my network") || HAS("network places") ||
        HAS("recycle") || HAS("control panel") || HAS("my pictures") ||
        HAS("my music") || HAS("computer"))
        return T_SYSTEM;
    /* Internet / browsers / comms */
    if (HAS("internet") || HAS("explorer") || HAS("firefox") || HAS("chrome") ||
        HAS("opera") || HAS("outlook") || HAS("mail") || HAS("msn") ||
        HAS("messenger") || HAS("browser") || HAS("edge"))
        return T_INTERNET;
    /* Games */
    if (HAS("quake") || HAS("unreal") || HAS("tribes") || HAS("wolfenstein") ||
        HAS("rtcw") || HAS("medal of honor") || HAS("mohaa") || HAS("descent") ||
        HAS("doom") || HAS("half-life") || HAS("counter-strike") || HAS("game") ||
        HAS("sin") || HAS("stargunner") || HAS("tyrian") || HAS("postal") ||
        HAS("serious sam") || HAS("deus ex") || HAS("nfs") || HAS("need for speed") ||
        HAS("play ") || HAS("launch"))
        return T_GAMES;
    /* Tools / utilities / dev / retro-agent */
    if (HAS("retro") || HAS("chat") || HAS("agent") || HAS("command") ||
        HAS("cmd") || HAS("prompt") || HAS("notepad") || HAS("setup") ||
        HAS("install") || HAS("config") || HAS("tool") || HAS("driver") ||
        HAS("benchmark") || HAS("3dmark") || HAS("regedit") || HAS("dxdiag"))
        return T_TOOLS;
    /* Media */
    if (HAS("media") || HAS("winamp") || HAS("vlc") || HAS("player") ||
        HAS("music") || HAS("video") || HAS("dvd") || HAS("quicktime") ||
        HAS("real") || HAS("photo"))
        return T_MEDIA;
    #undef HAS
    return T_OTHER;
}

typedef struct { int idx; int rank; wchar_t name[260]; } item_t;

static int cmp_item(const void *a, const void *b) {
    const item_t *x = a, *y = b;
    if (x->rank != y->rank) return x->rank - y->rank;
    return _wcsicmp(x->name, y->name);
}

int main(int argc, char **argv) {
    HWND lv = find_desktop_listview();
    if (!lv) { printf("desktop listview not found\n"); return 1; }

    /* turn off snap-to-grid + auto-arrange so our free positions stick */
    if (argc > 1 && strcmp(argv[1], "nogrid") == 0 && g_defview) {
        PostMessageA(g_defview, WM_COMMAND, FCIDM_SHVIEW_SNAPTOGRID, 0);
        Sleep(400);
    }
    LONG style = GetWindowLongA(lv, GWL_STYLE);
    if ((style & LVS_AUTOARRANGE) && g_defview) {
        SendMessageA(g_defview, WM_COMMAND, FCIDM_SHVIEW_AUTOARRANGE, 0);
        Sleep(200);
    }
    style = GetWindowLongA(lv, GWL_STYLE);
    SetWindowLongA(lv, GWL_STYLE, style & ~LVS_AUTOARRANGE);

    int n = (int)SendMessageA(lv, LVM_GETITEMCOUNT, 0, 0);
    if (n <= 0) { printf("no desktop icons\n"); return 0; }

    /* open explorer's process to read item text cross-process */
    DWORD pid = 0; GetWindowThreadProcessId(lv, &pid);
    HANDLE proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ |
                              PROCESS_VM_WRITE, FALSE, pid);
    const int TBUF = 260;
    LPVOID rtext = NULL, ritem = NULL;
    if (proc) {
        rtext = VirtualAllocEx(proc, NULL, TBUF * sizeof(wchar_t),
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        ritem = VirtualAllocEx(proc, NULL, sizeof(LVITEMW),
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    item_t *items = (item_t *)calloc(n, sizeof(item_t));
    int mycomp = -1;
    for (int i = 0; i < n; i++) {
        items[i].idx = i;
        items[i].name[0] = 0;
        if (proc && rtext && ritem) {
            LVITEMW li; memset(&li, 0, sizeof(li));
            li.mask = LVIF_TEXT; li.iItem = i; li.iSubItem = 0;
            li.pszText = (LPWSTR)rtext; li.cchTextMax = TBUF;
            WriteProcessMemory(proc, ritem, &li, sizeof(li), NULL);
            SendMessageA(lv, LVM_GETITEMTEXTW, i, (LPARAM)ritem);
            wchar_t buf[260]; SIZE_T got = 0;
            if (ReadProcessMemory(proc, rtext, buf, TBUF * sizeof(wchar_t), &got)) {
                buf[TBUF - 1] = 0;
                wcsncpy(items[i].name, buf, 259); items[i].name[259] = 0;
            }
        }
        /* "My Computer" (XP) / "Computer" (Vista+/localized 'This PC') sorts first */
        {
            wchar_t low[260]; int k;
            for (k = 0; items[i].name[k] && k < 259; k++) low[k] = towlower(items[i].name[k]);
            low[k] = 0;
            if (mycomp < 0 && (wcsstr(low, L"my computer") || wcscmp(low, L"computer") == 0 ||
                               wcsstr(low, L"this pc")))
                mycomp = i;
        }
        items[i].rank = classify(items[i].name);
    }
    if (mycomp >= 0) items[mycomp].rank = -1;   /* force My Computer first */

    qsort(items, n, sizeof(item_t), cmp_item);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int sx = 76, sy = 74;                 /* icon cell */
    int leftMargin = 12, bottomMargin = 52, topLimit = (int)(scrH * 0.30);

    /* how many rows fit from the bottom up to topLimit */
    int maxRows = (scrH - bottomMargin - topLimit) / sy;
    if (maxRows < 1) maxRows = 1;
    int cols = (n + maxRows - 1) / maxRows;
    if (cols < 1) cols = 1;
    int rows = (n + cols - 1) / cols;
    if (rows > maxRows) rows = maxRows;

    int startX = leftMargin;
    int startY = scrH - bottomMargin - rows * sy;
    if (startY < 0) startY = 0;

    /* column-major: My Computer at the TOP of the left column, themes flow down
       then into the next column to the right - a tidy bottom-left block. */
    for (int k = 0; k < n; k++) {
        int c = k / rows, r = k % rows;
        int x = startX + c * sx;
        int y = startY + r * sy;
        SendMessageA(lv, LVM_SETITEMPOSITION, items[k].idx, MAKELPARAM(x, y));
    }

    if (proc) {
        if (rtext) VirtualFreeEx(proc, rtext, 0, MEM_RELEASE);
        if (ritem) VirtualFreeEx(proc, ritem, 0, MEM_RELEASE);
        CloseHandle(proc);
    }
    printf("arranged %d icons bottom-left by theme (%d cols x %d rows), MyComputer=%s\n",
           n, cols, rows, mycomp >= 0 ? "first" : "not-found");
    free(items);
    return 0;
}
