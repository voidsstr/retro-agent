"""A directory listing that ends early is a failure, not the end (agent 1.85.0).

On .243 (Win98, 2026-09-25) the redirector dropped the SMB session mid-copy
(error 55). The file copy survived (1.84.2 reopens and seeks), but the
reconnect invalidated every open search handle, so FindNextFileA returned
FALSE and gs_copy_tree took that for "no more files". Quake2Win9x reported
"done: 4/52 title(s) copied, 0 file error(s)" with quake2.exe, every pak and
both launchers missing.

gs_copy_tree is compiled out of gamesync.c against a fake Win32 file API that
can fail a listing part-way, and exercised.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SRC = (REPO / "agent" / "src" / "gamesync.c").read_text(errors="replace")


def _extract(signature):
    start = SRC.index(signature)
    depth, i = 0, SRC.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(SRC[i], 0)
        if depth == 0:
            return SRC[start:i + 1]
        i += 1


HARNESS = r'''
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef unsigned long DWORD; typedef void *HANDLE; typedef long long __int64; typedef int BOOL;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct { DWORD dwFileAttributes; FILETIME ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; char cFileName[260]; } WIN32_FIND_DATAA;
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_NO_MORE_FILES 18
#define LOG_GS "GS"
#define _snprintf snprintf
typedef int CRITICAL_SECTION;
static CRITICAL_SECTION g_gs_lock;
static void EnterCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static void LeaveCriticalSection(CRITICAL_SECTION *c) { (void)c; }
static char *lstrcpynA(char *d, const char *s, int n) { strncpy(d, s, n - 1); d[n - 1] = 0; return d; }
static void Sleep(DWORD ms) { (void)ms; }
static void log_msg(const char *t, const char *f, ...) { (void)t; (void)f; }
static struct { int failed_files; char failed_file[260]; char file[260]; } g_gs;
static volatile int g_gs_abort = 0;
static DWORD g_err;
static DWORD GetLastError(void) { return g_err; }

/* fake share: dir "S" holds 12 files and a subdir "S\\sub" with 5 files */
static const char *list_for(const char *pat, int *n, int *isdir) {
    static const char *root[] = {"a","b","c","d","sub","e","f","g","h","i","j","k","l"};
    static const char *sub[] = {"s1","s2","s3","s4","s5"};
    (void)isdir;
    if (strstr(pat, "\\sub\\*")) { *n = 5; return (const char *)sub; }
    *n = 13; return (const char *)root;
}
typedef struct { const char **names; int n, pos; int is_sub; } find_t;
static int g_nexts = 0, g_fail_at = -1, g_fail_times = 0;
static HANDLE FindFirstFileA(const char *pat, WIN32_FIND_DATAA *fd) {
    find_t *f = calloc(1, sizeof(*f)); int n, isdir = 0;
    f->names = (const char **)list_for(pat, &n, &isdir); f->n = n; f->is_sub = strstr(pat, "\\sub\\*") != 0;
    memset(fd, 0, sizeof(*fd)); strcpy(fd->cFileName, f->names[0]);
    fd->dwFileAttributes = (!f->is_sub && !strcmp(f->names[0], "sub")) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    f->pos = 1; return f;
}
static BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) {
    find_t *f = h;
    g_nexts++;
    if (g_fail_times > 0 && g_nexts == g_fail_at) { g_fail_times--; g_fail_at += 7; g_err = 55; return 0; }
    if (f->pos >= f->n) { g_err = ERROR_NO_MORE_FILES; return 0; }
    memset(fd, 0, sizeof(*fd)); strcpy(fd->cFileName, f->names[f->pos]);
    fd->dwFileAttributes = (!f->is_sub && !strcmp(f->names[f->pos], "sub")) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    f->pos++; return 1;
}
static BOOL FindClose(HANDLE h) { free(h); return 1; }
static void gs_mkdir_p(const char *p) { (void)p; }
static char g_copied[64][MAX_PATH]; static int g_ncopied = 0;
static int gs_copy_file(const char *s, const char *d, __int64 sz, FILETIME *ft) {
    int i; (void)s; (void)sz; (void)ft;
    for (i = 0; i < g_ncopied; i++) if (!strcmp(g_copied[i], d)) return 1;   /* resume: already there */
    strcpy(g_copied[g_ncopied++], d); return 1;
}
%s
%s
int main(int c, char **v) {
    int r; g_fail_at = atoi(v[1]); g_fail_times = atoi(v[2]);
    r = gs_copy_tree("S", "D");
    printf("%%d %%d %%d\n", r, g_ncopied, g_gs.failed_files);
    return 0;
}
'''


@pytest.fixture(scope="module")
def run(tmp_path_factory):
    cc = shutil.which("gcc") or shutil.which("cc")
    if not cc:
        pytest.skip("no host C compiler - gs_copy_tree NOT exercised")
    define = re.search(r"#define GS_LIST_PASSES \d+", SRC).group(0)
    d = tmp_path_factory.mktemp("listing")
    (d / "t.c").write_text(HARNESS % (define, _extract("static int gs_copy_tree(")))
    subprocess.run([cc, "-w", "-o", str(d / "t"), str(d / "t.c")], check=True)

    def go(fail_at, times):
        out = subprocess.run([str(d / "t"), str(fail_at), str(times)], capture_output=True,
                             text=True, check=True).stdout.split()
        return tuple(int(x) for x in out)
    return go


ALL = 12 + 5


def test_a_clean_listing_copies_everything(run):
    assert run(-1, 0) == (1, ALL, 0)


def test_a_listing_cut_short_once_is_listed_again_and_nothing_is_missed(run):
    # the 3rd FindNextFile fails with error 55, as on .243
    ok, copied, failed = run(3, 1)
    assert copied == ALL, "every file must land even though the listing broke"
    assert (ok, failed) == (1, 0)


def test_a_listing_that_never_completes_is_a_recorded_failure(run):
    ok, copied, failed = run(2, 99)
    assert ok == 0 and failed >= 1, (
        "a directory that cannot be listed to the end must count as a failure, "
        "or gamesync.done certifies a half-copied title")


def test_the_loop_checks_why_the_listing_ended():
    body = _extract("static int gs_copy_tree(")
    assert "while (FindNextFileA(h, &fd));" in body
    after = body[body.index("while (FindNextFileA(h, &fd));"):]
    assert "GetLastError()" in after.split("FindClose(h);")[0], \
        "the reason must be read before FindClose can overwrite it"
    assert "ERROR_NO_MORE_FILES" in body
