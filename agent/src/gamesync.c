/*
 * gamesync.c - provision the game library onto a freshly installed box.
 *
 * WHY THIS EXISTS. The games used to ride along inside the XP image as an
 * $OEM$ payload, which text-mode setup copies to C: before the machine has
 * ever booted. That does not scale: the library is 6.4 GB today and growing,
 * setup copies it over SMB1 at the ~2.3 MB/s these Pentium III boxes actually
 * manage, and it all has to fit on a period disk. Worse, a single bad file in
 * the payload fails the whole OS install, because to setup it is not "a game
 * that did not copy", it is "a source file is missing".
 *
 * So the OS image is lean and this runs afterwards instead. By the time it
 * does, the machine has a real network stack, the agent is supervising the
 * copy, a failure costs one game rather than the install, and we can look at
 * the disk we actually landed on and skip what will not fit.
 *
 * FIRST-BOOT DETECTION is a marker file, not a registry flag: the agent has to
 * behave identically on 9x, where the Run key fires only at logon and nothing
 * supervises us. Marker absent means "new install, provision now".
 *
 * PROGRESS is logged continuously - overall percent, the title and file in
 * hand, and the measured transfer rate - because a 6 GB copy over SMB1 on a
 * 500 MHz machine takes a long while, and an operator watching a silent agent
 * cannot tell a slow copy from a wedged one. Speed comes from a sliding
 * window, not a cumulative average, so a stall shows up as the rate falling
 * rather than being hidden by a good first minute.
 *
 * The copy is resumable. Any file already present at the right size is
 * skipped, so a machine that lost power halfway does not start over.
 */

#include "handlers.h"
#include "protocol.h"
#include "util.h"
#include "log.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <shlobj.h>

#define LOG_GS "GAMESYNC"

/* The library the fleet publishes pre-installed game trees to. Overridable -
 * see gs_library_path() - because an address baked into a binary is a promise
 * we cannot keep across a NAS move. */
#define GS_DEFAULT_LIBRARY "\\\\192.168.1.122\\files\\Files\\Games-Library"
#define GS_DEST            "C:\\Games"
#define GS_MARKER          "C:\\RETRO_AGENT\\gamesync.done"
/* Written into the image by stage-oem.sh. Its PRESENCE is what says
 * "this machine was just imaged" - see gs_new_image(). */
#define GS_NEWIMAGE_FLAG   "C:\\RETRO_AGENT\\newimage.flag"
#define GS_INI             "C:\\RETRO_AGENT\\gamesync.ini"

#define GS_CHUNK           (64u * 1024u)
/* Leave the OS room to breathe; filling C: to the last byte breaks XP in
 * confusing ways long before it reports "disk full". */
#define GS_FREE_MARGIN     ((__int64)300 * 1024 * 1024)
#define GS_LOG_EVERY_MS    2000
#define GS_FIRST_DELAY_MS  20000

enum { GS_IDLE = 0, GS_SIZING, GS_COPYING, GS_DONE, GS_FAILED, GS_SKIPPED };

typedef struct {
    int     state;
    char    title[128];
    char    file[260];
    __int64 total_bytes;
    __int64 done_bytes;
    int     total_titles;
    int     done_titles;
    int     skipped_titles;
    int     failed_files;
    DWORD   started;
    double  mbps;
    char    message[256];
} gs_state_t;

static CRITICAL_SECTION g_gs_lock;
static int       g_gs_lock_ready;
static gs_state_t g_gs;
static volatile LONG g_gs_running;
static volatile LONG g_gs_abort;

/* Sliding-window rate measurement, touched only by the worker thread. */
static DWORD   g_win_tick;
static __int64 g_win_bytes;
static DWORD   g_last_log;

/* ---------------------------------------------------------------------- */
/* small helpers                                                           */
/* ---------------------------------------------------------------------- */

static void gs_set_msg(const char *fmt, ...)
{
    va_list ap;
    EnterCriticalSection(&g_gs_lock);
    va_start(ap, fmt);
    _vsnprintf(g_gs.message, sizeof(g_gs.message) - 1, fmt, ap);
    va_end(ap);
    g_gs.message[sizeof(g_gs.message) - 1] = 0;
    LeaveCriticalSection(&g_gs_lock);
}

static int gs_file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != 0xFFFFFFFF;
}

static __int64 gs_file_size(const char *path)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);
    __int64 sz;
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    sz = ((__int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    FindClose(h);
    return sz;
}

/* GetDiskFreeSpaceEx is absent on the earliest 95 builds, so bind it at run
 * time and fall back to the cluster-arithmetic call. Reporting "no idea" as
 * a huge number would be worse than useless here - it would let us fill the
 * disk - so a total failure to measure returns -1 and the caller copies
 * anyway rather than refusing everything. */
typedef BOOL (WINAPI *pGDFSE)(LPCSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER);

static __int64 gs_free_bytes(const char *root)
{
    static pGDFSE fn;
    static int    looked;
    ULARGE_INTEGER avail, total, freeb;
    DWORD spc, bps, freec, totalc;

    if (!looked) {
        HMODULE k = GetModuleHandleA("kernel32.dll");
        if (k)
            fn = (pGDFSE)GetProcAddress(k, "GetDiskFreeSpaceExA");
        looked = 1;
    }
    if (fn) {
        if (fn(root, &avail, &total, &freeb))
            return (__int64)avail.QuadPart;
    }
    if (GetDiskFreeSpaceA(root, &spc, &bps, &freec, &totalc))
        return (__int64)freec * (__int64)spc * (__int64)bps;
    return -1;
}

static void gs_mkdir_p(const char *path)
{
    char tmp[MAX_PATH];
    int  i, n;

    n = lstrlenA(path);
    if (n <= 0 || n >= (int)sizeof(tmp))
        return;
    lstrcpynA(tmp, path, sizeof(tmp));
    /* Skip the UNC or drive prefix so we never try to create "\\server". */
    i = (tmp[0] == '\\' && tmp[1] == '\\') ? 2 : 0;
    for (; tmp[i]; i++) {
        if (tmp[i] == '\\' && i > 2) {
            tmp[i] = 0;
            CreateDirectoryA(tmp, NULL);
            tmp[i] = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
}

static void gs_library_path(char *out, DWORD cch)
{
    HANDLE h;
    char   buf[512];
    DWORD  got = 0;
    char  *p;

    lstrcpynA(out, GS_DEFAULT_LIBRARY, cch);

    h = CreateFileA(GS_INI, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got) {
        buf[got] = 0;
        p = buf;
        while (*p) {
            if (str_starts_with(p, "library=")) {
                char *q = p + 8, *e;
                e = q;
                while (*e && *e != '\r' && *e != '\n')
                    e++;
                *e = 0;
                if (*q)
                    lstrcpynA(out, q, cch);
                break;
            }
            while (*p && *p != '\n')
                p++;
            if (*p)
                p++;
        }
    }
    CloseHandle(h);
}

/* ---------------------------------------------------------------------- */
/* sizing and copying                                                      */
/* ---------------------------------------------------------------------- */

static __int64 gs_dir_size(const char *dir, int *files)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char   pat[MAX_PATH], sub[MAX_PATH];
    __int64 total = 0;

    _snprintf(pat, sizeof(pat) - 1, "%s\\*", dir);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == '.' && fd.cFileName[2] == 0)))
            continue;
        _snprintf(sub, sizeof(sub) - 1, "%s\\%s", dir, fd.cFileName);
        sub[sizeof(sub) - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += gs_dir_size(sub, files);
        } else {
            total += ((__int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (files)
                (*files)++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return total;
}

/* added        - bytes now accounted for, whether transferred or skipped
 * transferred  - of those, how many actually crossed the wire
 *
 * The two differ because resume counts a file that is already present at the
 * right size as complete without reading it. Feeding those bytes into the rate
 * window produced readings like 760 MB/s over SMB1 - obvious nonsense on its
 * own, but the real cost is that a burst of skips inflates the average and can
 * hide a genuinely stalled transfer for the next few samples. Percentage counts
 * both; throughput counts only what moved. */
static void gs_note_progress2(__int64 added, __int64 transferred)
{
    DWORD now = GetTickCount();
    DWORD dt;
    __int64 total, done;
    int pct;
    char title[128], file[260];
    double mbps;

    EnterCriticalSection(&g_gs_lock);
    g_gs.done_bytes += added;
    total = g_gs.total_bytes;
    done  = g_gs.done_bytes;
    lstrcpynA(title, g_gs.title, sizeof(title));
    lstrcpynA(file,  g_gs.file,  sizeof(file));
    LeaveCriticalSection(&g_gs_lock);

    g_win_bytes += transferred;
    dt = now - g_win_tick;
    /* Recompute the rate about once a second. A cumulative average would let
     * a fast first minute mask a stall for a long time. */
    if (dt >= 1000) {
        mbps = ((double)g_win_bytes / 1048576.0) / ((double)dt / 1000.0);
        EnterCriticalSection(&g_gs_lock);
        g_gs.mbps = mbps;
        LeaveCriticalSection(&g_gs_lock);
        g_win_tick  = now;
        g_win_bytes = 0;
    }

    if (now - g_last_log >= GS_LOG_EVERY_MS) {
        g_last_log = now;
        EnterCriticalSection(&g_gs_lock);
        mbps = g_gs.mbps;
        LeaveCriticalSection(&g_gs_lock);
        pct = total > 0 ? (int)((done * 100) / total) : 0;
        log_msg(LOG_GS, "%3d%% | %I64d/%I64d MB | %.2f MB/s | %s | %s",
                pct, done / 1048576, total / 1048576, mbps, title, file);
    }
}

static void gs_note_progress(__int64 added)
{
    gs_note_progress2(added, added);
}

static int gs_copy_file(const char *src, const char *dst, __int64 src_size)
{
    HANDLE hs, hd;
    char  *buf;
    DWORD  rd, wr;
    int    ok = 1;
    __int64 already;

    /* Resume: an identical-sized file is treated as done. Size alone is a
     * weak check, but these are immutable release trees - and the alternative,
     * hashing 6 GB over SMB1 on a P3, costs more than it could ever save. */
    already = gs_file_size(dst);
    if (already >= 0 && already == src_size) {
        gs_note_progress2(src_size, 0);   /* counted, but nothing crossed the wire */
        return 1;
    }

    hs = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hs == INVALID_HANDLE_VALUE) {
        log_msg(LOG_GS, "open failed (%lu): %s", GetLastError(), src);
        return 0;
    }
    hd = CreateFileA(dst, GENERIC_WRITE, 0, NULL,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hd == INVALID_HANDLE_VALUE) {
        log_msg(LOG_GS, "create failed (%lu): %s", GetLastError(), dst);
        CloseHandle(hs);
        return 0;
    }
    buf = (char *)HeapAlloc(GetProcessHeap(), 0, GS_CHUNK);
    if (!buf) {
        CloseHandle(hs);
        CloseHandle(hd);
        return 0;
    }
    for (;;) {
        if (InterlockedExchange((LONG *)&g_gs_abort, g_gs_abort) != 0) {
            ok = 0;
            break;
        }
        if (!ReadFile(hs, buf, GS_CHUNK, &rd, NULL)) {
            log_msg(LOG_GS, "read failed (%lu): %s", GetLastError(), src);
            ok = 0;
            break;
        }
        if (rd == 0)
            break;
        if (!WriteFile(hd, buf, rd, &wr, NULL) || wr != rd) {
            log_msg(LOG_GS, "write failed (%lu): %s", GetLastError(), dst);
            ok = 0;
            break;
        }
        gs_note_progress((__int64)rd);
    }
    HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(hs);
    CloseHandle(hd);
    if (!ok)
        DeleteFileA(dst);      /* never leave a truncated file looking complete */
    return ok;
}

static int gs_copy_tree(const char *src, const char *dst)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char   pat[MAX_PATH], s[MAX_PATH], d[MAX_PATH];
    int    ok = 1;
    __int64 sz;

    gs_mkdir_p(dst);
    _snprintf(pat, sizeof(pat) - 1, "%s\\*", src);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == '.' && fd.cFileName[2] == 0)))
            continue;
        if (g_gs_abort)
            { ok = 0; break; }
        _snprintf(s, sizeof(s) - 1, "%s\\%s", src, fd.cFileName);
        _snprintf(d, sizeof(d) - 1, "%s\\%s", dst, fd.cFileName);
        s[sizeof(s) - 1] = 0;
        d[sizeof(d) - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!gs_copy_tree(s, d))
                ok = 0;
        } else {
            EnterCriticalSection(&g_gs_lock);
            lstrcpynA(g_gs.file, fd.cFileName, sizeof(g_gs.file));
            LeaveCriticalSection(&g_gs_lock);
            sz = ((__int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (!gs_copy_file(s, d, sz)) {
                EnterCriticalSection(&g_gs_lock);
                g_gs.failed_files++;
                LeaveCriticalSection(&g_gs_lock);
                ok = 0;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* desktop shortcuts                                                       */
/* ---------------------------------------------------------------------- */

/* A game copied to C:\Games is not much use if nobody can find it. Each title
 * ships launch.txt at its root - one line, the executable relative to the title
 * directory, optionally followed by a tab and a display name - and we turn that
 * into a desktop shortcut as soon as the title lands.
 *
 * Why a shipped file rather than guessing: the right executable is genuinely
 * not guessable. Red Alert 2's launcher is game.exe and NOT the ra2.exe sitting
 * beside it; Jedi Knight MotS is JKM.EXE, not the GOGLauncher.exe that looks
 * more like a launcher; Shogo's Shogo.exe is a front end for Client.exe. A
 * heuristic gets these wrong quietly, and the failure only shows up when
 * someone double-clicks and nothing happens.
 *
 * ole32 is bound at run time, like gameindex.c does, so a box where COM is
 * unavailable degrades to "no shortcut" rather than failing to start. */

static const GUID GS_CLSID_ShellLink =
    { 0x00021401, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };
static const GUID GS_IID_IShellLinkA =
    { 0x000214EE, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };
static const GUID GS_IID_IPersistFile =
    { 0x0000010B, 0x0000, 0x0000, { 0xC0,0,0,0,0,0,0,0x46 } };

typedef HRESULT (WINAPI *gs_coinit_t)(LPVOID);
typedef void    (WINAPI *gs_councoinit_t)(void);
typedef HRESULT (WINAPI *gs_cocreate_t)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef HRESULT (WINAPI *gs_shgetfolder_t)(HWND, int, HANDLE, DWORD, LPSTR);

static HMODULE          g_gs_ole32;
static HMODULE          g_gs_shell32;
static gs_coinit_t      g_gs_CoInitialize;
static gs_councoinit_t  g_gs_CoUninitialize;
static gs_cocreate_t    g_gs_CoCreateInstance;
static gs_shgetfolder_t g_gs_SHGetFolderPathA;

#define GS_CSIDL_DESKTOPDIRECTORY 0x0010
#define GS_CSIDL_COMMON_DESKTOPDIRECTORY 0x0019

static int gs_ole_load(void)
{
    if (g_gs_ole32)
        return g_gs_CoCreateInstance != NULL;
    g_gs_ole32 = LoadLibraryA("ole32.dll");
    if (!g_gs_ole32)
        return 0;
    g_gs_CoInitialize     = (gs_coinit_t)GetProcAddress(g_gs_ole32, "CoInitialize");
    g_gs_CoUninitialize   = (gs_councoinit_t)GetProcAddress(g_gs_ole32, "CoUninitialize");
    g_gs_CoCreateInstance = (gs_cocreate_t)GetProcAddress(g_gs_ole32, "CoCreateInstance");
    g_gs_shell32 = LoadLibraryA("shell32.dll");
    if (g_gs_shell32)
        g_gs_SHGetFolderPathA =
            (gs_shgetfolder_t)GetProcAddress(g_gs_shell32, "SHGetFolderPathA");
    return g_gs_CoCreateInstance != NULL;
}

/* Where shortcuts go. Prefer the ALL USERS desktop so the icons survive a
 * different account logging in - these boxes autologon as Administrator today,
 * but that is a setting, not a law. Falls back to the per-user desktop, then to
 * the 9x-era fixed path. */
static int gs_desktop_dir(char *out, DWORD cch)
{
    if (g_gs_SHGetFolderPathA) {
        if (SUCCEEDED(g_gs_SHGetFolderPathA(NULL, GS_CSIDL_COMMON_DESKTOPDIRECTORY,
                                            NULL, 0, out)) && out[0])
            return 1;
        if (SUCCEEDED(g_gs_SHGetFolderPathA(NULL, GS_CSIDL_DESKTOPDIRECTORY,
                                            NULL, 0, out)) && out[0])
            return 1;
    }
    {
        char win[MAX_PATH];
        if (GetWindowsDirectoryA(win, sizeof(win))) {
            _snprintf(out, cch - 1, "%s\\Desktop", win);
            out[cch - 1] = 0;
            if (gs_file_exists(out))
                return 1;
        }
    }
    return 0;
}

static int gs_make_shortcut(const char *target, const char *workdir,
                            const char *lnk_path, const char *desc)
{
    IShellLinkA  *sl = NULL;
    IPersistFile *pf = NULL;
    WCHAR         wpath[MAX_PATH];
    HRESULT       hr;
    int           ok = 0;

    if (!g_gs_CoCreateInstance)
        return 0;
    hr = g_gs_CoCreateInstance(&GS_CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                               &GS_IID_IShellLinkA, (void **)&sl);
    if (FAILED(hr) || !sl)
        return 0;

    sl->lpVtbl->SetPath(sl, target);
    sl->lpVtbl->SetWorkingDirectory(sl, workdir);
    /* The icon comes from the game's own exe, so the desktop shows the game's
     * artwork rather than a row of identical generic icons. */
    sl->lpVtbl->SetIconLocation(sl, target, 0);
    if (desc && desc[0])
        sl->lpVtbl->SetDescription(sl, desc);

    hr = sl->lpVtbl->QueryInterface(sl, &GS_IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr) && pf) {
        MultiByteToWideChar(CP_ACP, 0, lnk_path, -1, wpath, MAX_PATH);
        if (SUCCEEDED(pf->lpVtbl->Save(pf, wpath, TRUE)))
            ok = 1;
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
    return ok;
}

/* Read launch.txt: "<relative exe>[<TAB><display name>]" on the first line. */
static int gs_read_launch(const char *dir, char *exe, DWORD exe_cch,
                          char *name, DWORD name_cch)
{
    char   path[MAX_PATH];
    HANDLE h;
    char   buf[512];
    DWORD  got = 0;
    char  *tab, *end;

    exe[0] = name[0] = 0;
    _snprintf(path, sizeof(path) - 1, "%s\\launch.txt", dir);
    path[sizeof(path) - 1] = 0;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) || !got) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    buf[got] = 0;
    for (end = buf; *end && *end != '\r' && *end != '\n'; end++)
        ;
    *end = 0;
    tab = buf;
    while (*tab && *tab != '\t')
        tab++;
    if (*tab == '\t') {
        *tab = 0;
        lstrcpynA(name, tab + 1, name_cch);
    }
    lstrcpynA(exe, buf, exe_cch);
    return exe[0] != 0;
}

static void gs_make_game_shortcut(const char *dst_dir, const char *title)
{
    char exe_rel[MAX_PATH], disp[128], target[MAX_PATH];
    char desktop[MAX_PATH], lnk[MAX_PATH], workdir[MAX_PATH];
    char *slash;

    if (!gs_read_launch(dst_dir, exe_rel, sizeof(exe_rel), disp, sizeof(disp)))
        return;                       /* no launch.txt - nothing to point at */
    if (!disp[0])
        lstrcpynA(disp, title, sizeof(disp));

    _snprintf(target, sizeof(target) - 1, "%s\\%s", dst_dir, exe_rel);
    target[sizeof(target) - 1] = 0;
    if (!gs_file_exists(target)) {
        log_msg(LOG_GS, "%s: launch.txt names %s but it is not there - "
                        "no shortcut", title, exe_rel);
        return;
    }
    /* Working directory is the exe's own folder: many of these games look for
     * their data relative to the current directory and start in a broken state
     * if launched from elsewhere. */
    lstrcpynA(workdir, target, sizeof(workdir));
    slash = workdir + lstrlenA(workdir);
    while (slash > workdir && *slash != '\\')
        slash--;
    *slash = 0;

    if (!gs_ole_load() || !gs_desktop_dir(desktop, sizeof(desktop)))
        return;
    _snprintf(lnk, sizeof(lnk) - 1, "%s\\%s.lnk", desktop, disp);
    lnk[sizeof(lnk) - 1] = 0;

    if (gs_make_shortcut(target, workdir, lnk, disp))
        log_msg(LOG_GS, "%s: desktop shortcut -> %s", title, exe_rel);
    else
        log_msg(LOG_GS, "%s: could not create desktop shortcut", title);
}

/* ---------------------------------------------------------------------- */
/* desktop icon arrangement                                                */
/* ---------------------------------------------------------------------- */

/* Park the desktop icons in the wallpaper's icon bay.
 *
 * The wallpaper (scripts/retro-wallpaper/gen_retro_wall.py) draws a visible
 * slot for every icon position. THE GEOMETRY BELOW MUST MATCH ITS icon_bay()
 * EXACTLY - if the two drift, icons land between slots and the whole point of
 * the design is lost. tests/native/test_icon_bay.c pins them together.
 *
 * That drift is not hypothetical: the previous wallpaper reserved a well in the
 * bottom-LEFT while arrange_icons.exe parked icons in the bottom-RIGHT, so the
 * art and the icons sat on top of each other.
 *
 * Doing this in the agent rather than a staged helper exe matters on a fresh
 * image, where C:\retro-wall does not exist yet - a machine gets a tidy desktop
 * on its first boot rather than after someone remembers to stage a tool. */

#define LVM_FIRST_           0x1000
#define LVM_GETITEMCOUNT_    (LVM_FIRST_ + 4)
#define LVM_SETITEMPOSITION_ (LVM_FIRST_ + 15)
#define FCIDM_SHVIEW_AUTOARRANGE_ 0x7031
#ifndef LVS_AUTOARRANGE
#define LVS_AUTOARRANGE 0x0100
#endif

typedef struct { int x, y, cell_w, cell_h, cols, rows; } gs_bay_t;

/* Mirror of icon_bay() in gen_retro_wall.py. Keep the arithmetic identical. */
static void gs_icon_bay(int w, int h, gs_bay_t *b)
{
    int margin_x = (int)(w * 0.018);
    int margin_y = (int)(h * 0.030);
    const int header_h = 34;

    b->cell_w = 76;
    b->cell_h = 80;
    if (margin_x < 18) margin_x = 18;
    if (margin_y < 18) margin_y = 18;
    b->cols = (int)((w * 0.34) / b->cell_w);
    if (b->cols < 2) b->cols = 2;
    b->rows = (h - margin_y - header_h - 24) / b->cell_h;
    if (b->rows < 3) b->rows = 3;
    b->x = margin_x;
    b->y = margin_y + header_h;
}

static HWND gs_desktop_listview(HWND *defview_out)
{
    HWND prog = FindWindowA("Progman", NULL);
    HWND defview = FindWindowExA(prog, NULL, "SHELLDLL_DefView", NULL);
    if (!defview) {
        HWND worker = NULL;
        while ((worker = FindWindowExA(NULL, worker, "WorkerW", NULL)) != NULL) {
            defview = FindWindowExA(worker, NULL, "SHELLDLL_DefView", NULL);
            if (defview)
                break;
        }
    }
    if (defview_out)
        *defview_out = defview;
    if (!defview)
        return NULL;
    return FindWindowExA(defview, NULL, "SysListView32", NULL);
}

static void gs_arrange_icons(void)
{
    HWND     defview = NULL;
    HWND     lv = gs_desktop_listview(&defview);
    gs_bay_t bay;
    int      count, i, col, row;
    int      sw, sh;

    if (!lv) {
        log_msg(LOG_GS, "desktop listview not found - icons left as they are");
        return;
    }
    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    gs_icon_bay(sw, sh, &bay);

    /* Auto Arrange must be OFF or the shell snaps every icon back to the
     * top-left grid and our positions never stick.
     *
     * FCIDM_SHVIEW_AUTOARRANGE is a TOGGLE, not a set - so firing it blindly
     * turns the setting ON when it was already off, which is worse than doing
     * nothing and is precisely what happened: the icons ended up in neat rows
     * across the top of the screen instead of in the bay. Read the listview's
     * LVS_AUTOARRANGE style first (GetWindowLong works cross-process) and only
     * toggle when it is actually set.
     *
     * PostMessage rather than SendMessage because a synchronous send into the
     * shell can block us indefinitely. */
    if (defview) {
        LONG style = GetWindowLongA(lv, GWL_STYLE);
        if (style & LVS_AUTOARRANGE) {
            log_msg(LOG_GS, "auto-arrange is on - turning it off so icon "
                            "positions stick");
            PostMessageA(defview, WM_COMMAND, FCIDM_SHVIEW_AUTOARRANGE_, 0);
            Sleep(600);
            style = GetWindowLongA(lv, GWL_STYLE);
            if (style & LVS_AUTOARRANGE)
                log_msg(LOG_GS, "auto-arrange still on after the toggle - "
                                "icons may not stay where they are put");
        }
    }

    count = (int)SendMessageA(lv, LVM_GETITEMCOUNT_, 0, 0);
    if (count <= 0)
        return;

    for (i = 0; i < count; i++) {
        col = i % bay.cols;
        row = i / bay.cols;
        /* More icons than slots: keep packing downward rather than refusing.
         * A machine with a small screen and every game installed should still
         * get a tidy column, even if it runs past the drawn cells. */
        if (row >= bay.rows)
            row = bay.rows - 1 + (i / bay.cols - bay.rows + 1);
        /* +6 centres the icon in its drawn cell (cells are inset by 3 and the
         * icon's own bitmap is smaller than the cell). */
        SendMessageA(lv, LVM_SETITEMPOSITION_, (WPARAM)i,
                     MAKELPARAM(bay.x + col * bay.cell_w + 6,
                                bay.y + row * bay.cell_h + 6));
    }
    log_msg(LOG_GS, "arranged %d desktop icon(s) into the %dx%d icon bay",
            count, bay.cols, bay.rows);
}

/* ---------------------------------------------------------------------- */
/* per-title registry merge                                                */
/* ---------------------------------------------------------------------- */

/* Some games will not launch from a copied directory alone. Jedi Knight and
 * Mysteries of the Sith each open "<CD Path>\jk_.cd" as a disc-presence check
 * and refuse to start without the registry value pointing at the marker file
 * that ships in their tree; Red Alert 2 and others want an install path. A
 * title that needs this carries install.reg at its root, written against
 * C:\Games\<Title>, and we merge it here - immediately after that title's
 * files land, so a failure is attributable to the title rather than showing up
 * as a mysteriously broken game weeks later.
 *
 * Merging is best-effort by design: a game that fails to register is still
 * worth having on disk, and refusing to continue would cost the other twenty. */
static void gs_merge_reg(const char *dst_dir, const char *title)
{
    char reg_path[MAX_PATH];
    char cmd[MAX_PATH + 64];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;

    _snprintf(reg_path, sizeof(reg_path) - 1, "%s\\install.reg", dst_dir);
    reg_path[sizeof(reg_path) - 1] = 0;
    if (!gs_file_exists(reg_path))
        return;

    _snprintf(cmd, sizeof(cmd) - 1, "regedit /s \"%s\"", reg_path);
    cmd[sizeof(cmd) - 1] = 0;

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        log_msg(LOG_GS, "%s: cannot run regedit (%lu) - game may not launch",
                title, GetLastError());
        return;
    }
    /* regedit /s is quick, but never wait forever on it. */
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_TIMEOUT)
        log_msg(LOG_GS, "%s: regedit still running after 60s, leaving it", title);
    else if (GetExitCodeProcess(pi.hProcess, &code) && code != 0)
        log_msg(LOG_GS, "%s: regedit exited %lu", title, code);
    else
        log_msg(LOG_GS, "%s: merged install.reg", title);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

/* ---------------------------------------------------------------------- */
/* the run                                                                 */
/* ---------------------------------------------------------------------- */

static void gs_write_marker(int titles)
{
    HANDLE h;
    char   line[256];
    DWORD  wr;
    SYSTEMTIME st;

    gs_mkdir_p("C:\\RETRO_AGENT");
    h = CreateFileA(GS_MARKER, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    GetLocalTime(&st);
    _snprintf(line, sizeof(line) - 1,
              "provisioned %04d-%02d-%02d %02d:%02d:%02d titles=%d\r\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
              titles);
    line[sizeof(line) - 1] = 0;
    WriteFile(h, line, (DWORD)lstrlenA(line), &wr, NULL);
    CloseHandle(h);
}

static void gs_run(const char *library)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char   pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    char   titles[64][128];
    __int64 sizes[64];
    int    n = 0, i, files = 0, ok_titles = 0;
    __int64 grand = 0, freeb;

    g_gs_abort = 0;
    g_win_tick = GetTickCount();
    g_win_bytes = 0;
    g_last_log = 0;

    EnterCriticalSection(&g_gs_lock);
    memset(&g_gs, 0, sizeof(g_gs));
    g_gs.state   = GS_SIZING;
    g_gs.started = GetTickCount();
    LeaveCriticalSection(&g_gs_lock);

    log_msg(LOG_GS, "library: %s", library);
    gs_set_msg("enumerating library");

    _snprintf(pat, sizeof(pat) - 1, "%s\\*", library);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        log_msg(LOG_GS, "cannot reach library (%lu): %s", GetLastError(), library);
        EnterCriticalSection(&g_gs_lock);
        g_gs.state = GS_FAILED;
        LeaveCriticalSection(&g_gs_lock);
        gs_set_msg("library unreachable - will retry");
        return;
    }
    do {
        if (fd.cFileName[0] == '.')
            continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (n >= 64)
            break;
        lstrcpynA(titles[n], fd.cFileName, sizeof(titles[0]));
        _snprintf(src, sizeof(src) - 1, "%s\\%s", library, fd.cFileName);
        src[sizeof(src) - 1] = 0;
        sizes[n] = gs_dir_size(src, &files);
        grand += sizes[n];
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (n == 0) {
        log_msg(LOG_GS, "library is empty - nothing to do");
        EnterCriticalSection(&g_gs_lock);
        g_gs.state = GS_DONE;
        LeaveCriticalSection(&g_gs_lock);
        gs_write_marker(0);
        return;
    }

    freeb = gs_free_bytes("C:\\");
    log_msg(LOG_GS, "%d title(s), %d file(s), %I64d MB to copy; C: has %I64d MB free",
            n, files, grand / 1048576,
            freeb < 0 ? (__int64)-1 : freeb / 1048576);

    EnterCriticalSection(&g_gs_lock);
    g_gs.state        = GS_COPYING;
    g_gs.total_titles = n;
    g_gs.total_bytes  = grand;
    LeaveCriticalSection(&g_gs_lock);

    gs_mkdir_p(GS_DEST);

    for (i = 0; i < n; i++) {
        if (g_gs_abort) {
            log_msg(LOG_GS, "aborted by request");
            break;
        }
        /* Re-measure per title: earlier titles have just consumed space, and
         * on a period disk the difference decides whether this one fits. */
        freeb = gs_free_bytes("C:\\");
        if (freeb >= 0 && sizes[i] + GS_FREE_MARGIN > freeb) {
            log_msg(LOG_GS, "SKIP %s - needs %I64d MB, only %I64d MB free",
                    titles[i], sizes[i] / 1048576, freeb / 1048576);
            EnterCriticalSection(&g_gs_lock);
            g_gs.skipped_titles++;
            /* Its bytes are never going to arrive; drop them from the target
             * so the overall percentage still reaches 100. */
            g_gs.total_bytes -= sizes[i];
            LeaveCriticalSection(&g_gs_lock);
            continue;
        }
        EnterCriticalSection(&g_gs_lock);
        lstrcpynA(g_gs.title, titles[i], sizeof(g_gs.title));
        g_gs.file[0] = 0;
        LeaveCriticalSection(&g_gs_lock);
        gs_set_msg("copying %s", titles[i]);
        log_msg(LOG_GS, "==> %s (%I64d MB)", titles[i], sizes[i] / 1048576);

        _snprintf(src, sizeof(src) - 1, "%s\\%s", library, titles[i]);
        _snprintf(dst, sizeof(dst) - 1, "%s\\%s", GS_DEST, titles[i]);
        src[sizeof(src) - 1] = 0;
        dst[sizeof(dst) - 1] = 0;

        if (gs_copy_tree(src, dst)) {
            ok_titles++;
            gs_merge_reg(dst, titles[i]);
            gs_make_game_shortcut(dst, titles[i]);
        } else {
            log_msg(LOG_GS, "%s finished with errors", titles[i]);
        }

        EnterCriticalSection(&g_gs_lock);
        g_gs.done_titles++;
        LeaveCriticalSection(&g_gs_lock);
    }

    EnterCriticalSection(&g_gs_lock);
    g_gs.state = g_gs_abort ? GS_FAILED : GS_DONE;
    i = g_gs.failed_files;
    LeaveCriticalSection(&g_gs_lock);

    /* Arrange AFTER every shortcut exists: the shell creates a listview item
     * per .lnk asynchronously, so arranging per title would keep re-sorting a
     * list that is still growing. */
    Sleep(2000);
    gs_arrange_icons();

    log_msg(LOG_GS, "done: %d/%d title(s) copied, %d skipped, %d file error(s)",
            ok_titles, n, g_gs.skipped_titles, i);
    gs_set_msg("complete - %d title(s)", ok_titles);

    /* Only claim the box is provisioned if nothing failed. A marker written
     * over a partial run would make the next boot skip the retry. */
    if (!g_gs_abort && i == 0)
        gs_write_marker(ok_titles);
}

/* ---------------------------------------------------------------------- */
/* thread and handler                                                      */
/* ---------------------------------------------------------------------- */

typedef struct { char library[MAX_PATH]; } gs_arg_t;

static DWORD WINAPI gs_worker(LPVOID param)
{
    gs_arg_t *a = (gs_arg_t *)param;
    char lib[MAX_PATH];

    lstrcpynA(lib, a->library, sizeof(lib));
    HeapFree(GetProcessHeap(), 0, a);

    /* COM must be initialised on the thread that uses it. Do it around the
     * whole run rather than per shortcut - CoInitialize is cheap but the
     * apartment has to outlive every interface pointer we hold. */
    if (gs_ole_load() && g_gs_CoInitialize)
        g_gs_CoInitialize(NULL);
    gs_run(lib);
    if (g_gs_CoUninitialize)
        g_gs_CoUninitialize();
    InterlockedExchange((LONG *)&g_gs_running, 0);
    return 0;
}

static int gs_start(const char *library)
{
    gs_arg_t *a;
    HANDLE    th;
    DWORD     tid;

    if (InterlockedCompareExchange((LONG *)&g_gs_running, 1, 0) != 0)
        return 0;                          /* already running */

    a = (gs_arg_t *)HeapAlloc(GetProcessHeap(), 0, sizeof(gs_arg_t));
    if (!a) {
        InterlockedExchange((LONG *)&g_gs_running, 0);
        return 0;
    }
    if (library && library[0])
        lstrcpynA(a->library, library, sizeof(a->library));
    else
        gs_library_path(a->library, sizeof(a->library));

    th = CreateThread(NULL, 0, gs_worker, a, 0, &tid);
    if (!th) {
        HeapFree(GetProcessHeap(), 0, a);
        InterlockedExchange((LONG *)&g_gs_running, 0);
        return 0;
    }
    CloseHandle(th);
    return 1;
}

void gamesync_init(void)
{
    if (!g_gs_lock_ready) {
        InitializeCriticalSection(&g_gs_lock);
        g_gs_lock_ready = 1;
    }
}

/* Report what the image left behind, so the log says which build a box came
 * from rather than just that it is new. Best-effort: the flag is small and
 * plain text, and a missing or unreadable one is not an error. */
static void gs_log_image_flag(void)
{
    HANDLE h;
    char   buf[256];
    DWORD  got = 0;
    char  *p;

    h = CreateFileA(GS_NEWIMAGE_FLAG, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got) {
        buf[got] = 0;
        for (p = buf; *p; p++)
            if (*p == '\r' || *p == '\n')
                *p = ' ';
        log_msg(LOG_GS, "image flag: %s", buf);
    }
    CloseHandle(h);
}

DWORD WINAPI gamesync_thread(LPVOID param)
{
    int fresh;

    (void)param;
    /* Let the desktop settle and the redirector come up before touching a
     * UNC path; on a fresh XP logon the network is not ready immediately. */
    Sleep(GS_FIRST_DELAY_MS);

    /* Two independent signals, and they answer different questions.
     *
     *   newimage.flag  is placed BY THE IMAGE, so its presence is positive
     *                  evidence that this box was just installed.
     *   gamesync.done  is written by US once a run completes cleanly.
     *
     * Absence of the done-marker alone is a weak signal - it is also absent if
     * someone deleted it, or on a box that predates this feature entirely. The
     * flag is what lets the log say 'freshly imaged' and mean it. */
    fresh = gs_file_exists(GS_NEWIMAGE_FLAG);
    if (fresh)
        gs_log_image_flag();

    if (gs_file_exists(GS_MARKER)) {
        log_msg(LOG_GS, "already provisioned (%s present) - idle", GS_MARKER);
        return 0;
    }
    log_msg(LOG_GS, fresh
            ? "FRESHLY IMAGED machine - provisioning game library"
            : "no provisioning marker - provisioning game library");
    for (;;) {
        if (gs_file_exists(GS_MARKER))
            return 0;
        if (!g_gs_running)
            gs_start(NULL);
        /* If the library was unreachable the run failed fast; wait before
         * trying again rather than hammering a NAS that may still be waking. */
        Sleep(120000);
    }
}

void handle_gamesync(SOCKET sock, const char *args)
{
    const char *a = str_skip_spaces(args ? args : "");
    char   json[1024];
    gs_state_t s;
    const char *names[] = { "idle", "sizing", "copying", "done", "failed", "skipped" };
    int    pct, elapsed;
    char   lib[MAX_PATH];

    gamesync_init();

    if (str_starts_with(a, "START")) {
        const char *unc = str_skip_spaces(a + 5);
        if (gs_start(unc[0] ? unc : NULL))
            send_text_response(sock, "OK started");
        else
            send_text_response(sock, "OK already running");
        return;
    }
    if (str_starts_with(a, "ABORT")) {
        InterlockedExchange((LONG *)&g_gs_abort, 1);
        send_text_response(sock, "OK aborting");
        return;
    }
    if (str_starts_with(a, "RESET")) {
        /* Forget that this box was provisioned, so the next start re-runs. */
        DeleteFileA(GS_MARKER);
        send_text_response(sock, "OK marker cleared");
        return;
    }
    if (str_starts_with(a, "LIBRARY")) {
        gs_library_path(lib, sizeof(lib));
        send_text_response(sock, lib);
        return;
    }

    EnterCriticalSection(&g_gs_lock);
    s = g_gs;
    LeaveCriticalSection(&g_gs_lock);

    pct = s.total_bytes > 0 ? (int)((s.done_bytes * 100) / s.total_bytes) : 0;
    elapsed = s.started ? (int)((GetTickCount() - s.started) / 1000) : 0;

    _snprintf(json, sizeof(json) - 1,
        "{\"state\":\"%s\",\"percent\":%d,"
        "\"titles_done\":%d,\"titles_total\":%d,\"titles_skipped\":%d,"
        "\"mb_done\":%I64d,\"mb_total\":%I64d,\"mbps\":%.2f,"
        "\"current_title\":\"%s\",\"current_file\":\"%s\","
        "\"failed_files\":%d,\"elapsed_s\":%d,\"provisioned\":%s,"
        "\"new_image\":%s,\"message\":\"%s\"}",
        names[(s.state >= 0 && s.state <= GS_SKIPPED) ? s.state : 0],
        pct, s.done_titles, s.total_titles, s.skipped_titles,
        s.done_bytes / 1048576, s.total_bytes / 1048576, s.mbps,
        s.title, s.file, s.failed_files, elapsed,
        gs_file_exists(GS_MARKER) ? "true" : "false",
        gs_file_exists(GS_NEWIMAGE_FLAG) ? "true" : "false",
        s.message);
    /* new_image is deliberately reported alongside provisioned: together they
     * distinguish "fresh box, not yet done" from "old box someone reset". */
    json[sizeof(json) - 1] = 0;
    send_text_response(sock, json);
}
