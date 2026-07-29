/* dosstage_env.h — minimal fake Win32 so the REAL agent/src/dosstage.c can be
 * compiled and executed natively by test_dosstage.c.
 *
 * dosstage.c is the module that decides whether a fleet box gets the DOS
 * programs staged to C:\ and which files it copies. That decision is what we
 * want under test, not Microsoft's file APIs — so the registry, file, and
 * thread calls it makes are redirected here to in-memory fakes the test can
 * program (set the platform id, seed share/local file sizes) and inspect
 * (which files were copied, in what order, with what pacing).
 *
 * Only what dosstage.c actually touches is modelled. Keep it that way: this
 * is a test harness, not a Win32 implementation.
 */

#ifndef DOSSTAGE_ENV_H
#define DOSSTAGE_ENV_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- basic types ---- */
typedef unsigned char       BYTE;
typedef unsigned long       DWORD;
typedef int                 BOOL;
typedef unsigned short      WORD;
typedef void               *HANDLE;
typedef void               *LPVOID;
typedef void               *HKEY;
typedef const char         *LPCSTR;
typedef unsigned long       UINT_PTR;
typedef long                LONG;
typedef int                 SOCKET;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);

#define WINAPI
#define MAX_PATH 260
#define TRUE  1
#define FALSE 0

#define ERROR_SUCCESS       0L
#define KEY_READ            1
#define KEY_WRITE           2
#define REG_DWORD           4
#define REG_SZ              1
#define HKEY_LOCAL_MACHINE  ((HKEY)0x80000002)

#define VER_PLATFORM_WIN32_WINDOWS 1
#define VER_PLATFORM_WIN32_NT      2

#define INVALID_HANDLE_VALUE      ((HANDLE)-1)
#define DRIVE_UNKNOWN   0
#define DRIVE_FIXED     3
#define DRIVE_REMOTE    4
#define FILE_ATTRIBUTE_DIRECTORY  0x10
#define THREAD_PRIORITY_BELOW_NORMAL (-1)

typedef struct {
    DWORD dwOSVersionInfoSize;
    DWORD dwMajorVersion;
    DWORD dwMinorVersion;
    DWORD dwBuildNumber;
    DWORD dwPlatformId;
    char  szCSDVersion[128];
} OSVERSIONINFOA;

typedef struct {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    DWORD dwTotalPhys;
    DWORD dwAvailPhys;
    DWORD dwTotalPageFile;
    DWORD dwAvailPageFile;
    DWORD dwTotalVirtual;
    DWORD dwAvailVirtual;
} MEMORYSTATUS;

typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

typedef struct {
    DWORD dwFileAttributes;
    DWORD nFileSizeLow;
    char  cFileName[MAX_PATH];
} WIN32_FIND_DATAA;

/* ---- programmable fake state ---- */

#define FAKE_MAX_FILES 64

typedef struct {
    char  path[MAX_PATH];       /* full path, backslash separated */
    DWORD size;
    int   is_dir;
} fake_file_t;

typedef struct {
    DWORD platform_id;          /* VER_PLATFORM_WIN32_* */
    int   reg_enable_present;   /* is DosStage present? */
    DWORD reg_enable_value;
    int   reg_tiles_present;    /* is DosStageTiles present? */
    DWORD reg_tiles_value;
    DWORD avail_mb;             /* free physical memory the box reports */
    char  reg_path[512];        /* DosStagePath ("" = absent) */
    char  marker[128];          /* DosStaged written back */

    fake_file_t files[FAKE_MAX_FILES];
    int   n_files;

    /* observations */
    char  copied[FAKE_MAX_FILES][MAX_PATH];   /* destinations, in order */
    int   n_copied;
    int   sleeps[FAKE_MAX_FILES];             /* Sleep() calls, in order */
    int   n_sleeps;
    int   dirs_created;
    int   thread_priority;
    /* Which drive letters report as network drives ("D" -> D: is remote).
     * Everything else answers DRIVE_FIXED, like a local disk. */
    char  remote_drives[27];
} fake_env_t;

extern fake_env_t g_env;

/* Seed a file that "exists" (on the share or locally). */
static void fake_add_file(const char *path, DWORD size)
{
    fake_file_t *f = &g_env.files[g_env.n_files++];
    strncpy(f->path, path, MAX_PATH - 1);
    f->path[MAX_PATH - 1] = '\0';
    f->size = size;
    f->is_dir = 0;
}

static fake_file_t *fake_find(const char *path)
{
    int i;
    for (i = 0; i < g_env.n_files; i++)
        if (strcmp(g_env.files[i].path, path) == 0) return &g_env.files[i];
    return NULL;
}

/* ---- fake Win32 ---- */

static BOOL GetVersionExA(OSVERSIONINFOA *osvi)
{
    osvi->dwPlatformId = g_env.platform_id;
    osvi->dwMajorVersion = 4;
    osvi->dwMinorVersion = 10;
    return TRUE;
}

static DWORD RegOpenKeyExA(HKEY root, LPCSTR sub, DWORD opt, DWORD acc, HKEY *out)
{
    (void)root; (void)sub; (void)opt; (void)acc;
    *out = (HKEY)1;
    return ERROR_SUCCESS;
}

static DWORD RegCreateKeyExA(HKEY root, LPCSTR sub, DWORD r, void *cls, DWORD o,
                             DWORD acc, void *sa, HKEY *out, DWORD *disp)
{
    (void)root; (void)sub; (void)r; (void)cls; (void)o; (void)acc; (void)sa;
    (void)disp;
    *out = (HKEY)1;
    return ERROR_SUCCESS;
}

static DWORD RegQueryValueExA(HKEY k, LPCSTR name, void *res, DWORD *type,
                              BYTE *data, DWORD *size)
{
    (void)k; (void)res;
    if (strcmp(name, "DosStage") == 0) {
        if (!g_env.reg_enable_present) return 2;   /* ERROR_FILE_NOT_FOUND */
        *type = REG_DWORD;
        memcpy(data, &g_env.reg_enable_value, sizeof(DWORD));
        *size = sizeof(DWORD);
        return ERROR_SUCCESS;
    }
    if (strcmp(name, "DosStageTiles") == 0) {
        if (!g_env.reg_tiles_present) return 2;
        *type = REG_DWORD;
        memcpy(data, &g_env.reg_tiles_value, sizeof(DWORD));
        *size = sizeof(DWORD);
        return ERROR_SUCCESS;
    }
    if (strcmp(name, "DosStagePath") == 0) {
        if (!g_env.reg_path[0]) return 2;
        *type = REG_SZ;
        strcpy((char *)data, g_env.reg_path);
        *size = (DWORD)strlen(g_env.reg_path) + 1;
        return ERROR_SUCCESS;
    }
    return 2;
}

static DWORD RegSetValueExA(HKEY k, LPCSTR name, DWORD r, DWORD type,
                            const BYTE *data, DWORD size)
{
    (void)k; (void)r; (void)type; (void)size;
    if (strcmp(name, "DosStaged") == 0) {
        strncpy(g_env.marker, (const char *)data, sizeof(g_env.marker) - 1);
        g_env.marker[sizeof(g_env.marker) - 1] = '\0';
    }
    return ERROR_SUCCESS;
}

static DWORD RegCloseKey(HKEY k) { (void)k; return ERROR_SUCCESS; }

/* Directory enumeration: a find handle walks the seeded file table matching
 * "<dir>\*.*" — enough for dosstage.c's copy_dir(). */
typedef struct { char dir[MAX_PATH]; int next; } fake_find_t;
static fake_find_t g_finds[8];
static int g_n_finds = 0;

static int fake_in_dir(const char *path, const char *dir, char *name_out)
{
    size_t dl = strlen(dir);
    const char *rest;
    if (strncmp(path, dir, dl) != 0 || path[dl] != '\\') return 0;
    rest = path + dl + 1;
    if (strchr(rest, '\\')) return 0;          /* deeper: not this dir */
    strncpy(name_out, rest, MAX_PATH - 1);
    name_out[MAX_PATH - 1] = '\0';
    return 1;
}

static HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd);
static BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);

static BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd)
{
    fake_find_t *f = (fake_find_t *)h;
    while (f->next < g_env.n_files) {
        fake_file_t *ff = &g_env.files[f->next++];
        char name[MAX_PATH];
        if (fake_in_dir(ff->path, f->dir, name)) {
            memset(fd, 0, sizeof(*fd));
            strncpy(fd->cFileName, name, MAX_PATH - 1);
            fd->nFileSizeLow = ff->size;
            fd->dwFileAttributes = ff->is_dir ? FILE_ATTRIBUTE_DIRECTORY : 0;
            return TRUE;
        }
    }
    return FALSE;
}

static HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd)
{
    char p[MAX_PATH];
    char *star;
    fake_find_t *f;

    strncpy(p, pattern, MAX_PATH - 1);
    p[MAX_PATH - 1] = '\0';

    star = strstr(p, "\\*.*");
    if (!star) {
        /* single-file query (file_size_of) */
        fake_file_t *ff = fake_find(p);
        if (!ff) return INVALID_HANDLE_VALUE;
        memset(fd, 0, sizeof(*fd));
        fd->nFileSizeLow = ff->size;
        strncpy(fd->cFileName, p, MAX_PATH - 1);
        /* one-shot handle: a subsequent FindNextFileA must return FALSE */
        f = &g_finds[g_n_finds++ % 8];
        f->dir[0] = '\0';
        f->next = g_env.n_files;
        return (HANDLE)f;
    }

    *star = '\0';
    f = &g_finds[g_n_finds++ % 8];
    strncpy(f->dir, p, MAX_PATH - 1);
    f->dir[MAX_PATH - 1] = '\0';
    f->next = 0;
    if (!FindNextFileA((HANDLE)f, fd)) return INVALID_HANDLE_VALUE;
    return (HANDLE)f;
}

static BOOL FindClose(HANDLE h) { (void)h; return TRUE; }

static BOOL CopyFileA(LPCSTR src, LPCSTR dst, BOOL fail_if_exists)
{
    fake_file_t *s = fake_find(src);
    fake_file_t *d;
    (void)fail_if_exists;
    if (!s) return FALSE;
    strncpy(g_env.copied[g_env.n_copied], dst, MAX_PATH - 1);
    g_env.copied[g_env.n_copied][MAX_PATH - 1] = '\0';
    g_env.n_copied++;
    d = fake_find(dst);
    if (d) d->size = s->size;
    else fake_add_file(dst, s->size);
    return TRUE;
}

static BOOL CreateDirectoryA(LPCSTR path, void *sa)
{
    (void)path; (void)sa;
    g_env.dirs_created++;
    return TRUE;
}

static void Sleep(DWORD ms)
{
    if (g_env.n_sleeps < FAKE_MAX_FILES) g_env.sleeps[g_env.n_sleeps++] = (int)ms;
}

static void GlobalMemoryStatus(MEMORYSTATUS *ms)
{
    ms->dwAvailPhys = g_env.avail_mb * 1024UL * 1024UL;
    ms->dwTotalPhys = 32UL * 1024UL * 1024UL;
}

static DWORD GetDriveTypeA(LPCSTR root)
{
    if (root && root[0] && strchr(g_env.remote_drives, root[0]))
        return DRIVE_REMOTE;
    return DRIVE_FIXED;
}

static DWORD GetLastError(void) { return 0; }
static void GetLocalTime(SYSTEMTIME *st) { memset(st, 0, sizeof(*st)); st->wYear = 2026; }
static HANDLE GetCurrentThread(void) { return (HANDLE)1; }
static BOOL SetThreadPriority(HANDLE h, int pri)
{
    (void)h;
    g_env.thread_priority = pri;
    return TRUE;
}
static HANDLE CreateThread(void *sa, DWORD stack, LPTHREAD_START_ROUTINE fn,
                           LPVOID param, DWORD flags, DWORD *tid)
{
    (void)sa; (void)stack; (void)fn; (void)param; (void)flags; (void)tid;
    return (HANDLE)1;   /* tests call the run function directly */
}
static BOOL CloseHandle(HANDLE h) { (void)h; return TRUE; }
/* Watcom/MSVC spelling used throughout the agent sources */
#define _snprintf snprintf

#endif /* DOSSTAGE_ENV_H */
