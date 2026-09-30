/* menudef9x - point C:\CONFIG.SYS's boot-menu default at one block, for ONE boot.
 *
 *   menudef9x <block>        e.g.  menudef9x EMS
 *   exit 0 = set (or already set), 1 = no menudefault= line, 2 = no [<block>] block,
 *        3 = CONFIG.SYS unreadable/too big, 4 = the write did not read back
 *   log: C:\RUNDOS\MENUDEF.TXT (truncated each run)
 *
 * WHY (2026-09-30). A staged real-DOS title (scripts/dosgames/stage_win9x_dos.py
 * real_dos) restarts the PC and runs from the one-shot rundos line in
 * AUTOEXEC.BAT. Falcon 3.0 and Pacific Strike need EXPANDED memory, which real
 * DOS has only when CONFIG.SYS loaded EMM386 - on .243 that is its boot menu's
 * item 2, and the menu's default is the XMS-only item 1 on purpose. So the
 * launcher copies C:\CONFIG.SYS to C:\RUNDOS\CONFIG.SAV, runs this to make the
 * EMS block the default, and restarts; the rundos line puts CONFIG.SAV back
 * FIRST at the next boot, before the game runs, so a game that hangs the PC
 * cannot leave the box booting EMS.
 *
 * Only the block name on the menudefault= line changes (the timeout after the
 * comma and every other byte stay), and only when [<block>] exists. The new
 * bytes are written in place and read back; anything unexpected leaves the
 * file as it was.
 *
 * No C runtime. Build:
 *   i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -fno-builtin -e _start@0 \
 *       -o menudef9x.exe menudef9x.c -lkernel32 -luser32 -s
 */
#include <windows.h>

#define CFG     "C:\\CONFIG.SYS"
#define LOGF    "C:\\RUNDOS\\MENUDEF.TXT"
#define MAXCFG  65536

static HANDLE g_log = INVALID_HANDLE_VALUE;
static char   g_buf[MAXCFG + 64], g_new[MAXCFG + 64], g_back[MAXCFG + 64];

static void say(const char *s)
{
    DWORD n;
    if (g_log == INVALID_HANDLE_VALUE) return;
    WriteFile(g_log, s, lstrlenA(s), &n, NULL);
    WriteFile(g_log, "\r\n", 2, &n, NULL);
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static void copy(char *d, const char *s, int n) { while (n-- > 0) *d++ = *s++; }

/* Does the line at p (up to CR/LF) start, after blanks, with word + optional blanks + '='?
 * Returns the offset of the first byte after '=' and blanks, or -1. */
static int key_value(const char *p, int len, const char *word)
{
    int i = 0, j = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    for (; word[j]; j++, i++)
        if (i >= len || lower((unsigned char)p[i]) != lower((unsigned char)word[j])) return -1;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    if (i >= len || p[i] != '=') return -1;
    i++;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    return i;
}

/* Is the line exactly "[block]" (case-insensitive, blanks around allowed)? */
static int is_block(const char *p, int len, const char *block)
{
    int i = 0, j = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    if (i >= len || p[i] != '[') return 0;
    i++;
    for (; block[j]; j++, i++)
        if (i >= len || lower((unsigned char)p[i]) != lower((unsigned char)block[j])) return 0;
    if (i >= len || p[i] != ']') return 0;
    i++;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    return i == len;
}

static int next_arg(const char **pp, char *out, int cap)
{
    const char *s = *pp;
    int n = 0, q = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) { *pp = s; return 0; }
    if (*s == '"') { q = 1; s++; }
    while (*s && (q ? *s != '"' : (*s != ' ' && *s != '\t'))) {
        if (n < cap - 1) out[n++] = *s;
        s++;
    }
    if (q && *s == '"') s++;
    out[n] = 0;
    *pp = s;
    return 1;
}

void WINAPI _start(void)
{
    const char *cmd = GetCommandLineA();
    char exe[MAX_PATH], block[32], line[160];
    HANDLE h;
    DWORD n = 0, w = 0, size;
    int i, rc = 3, have_block = 0, vstart = -1, vend = -1, start, len, blen, nlen;

    CreateDirectoryA("C:\\RUNDOS", NULL);
    g_log = CreateFileA(LOGF, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    next_arg(&cmd, exe, sizeof(exe));
    if (!next_arg(&cmd, block, sizeof(block)) || !block[0]) {
        say("usage: menudef9x <block>");
        rc = 3;
        goto out;
    }
    h = CreateFileA(CFG, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { say("FAIL: C:\\CONFIG.SYS cannot be opened"); goto out; }
    size = GetFileSize(h, NULL);
    if (size == 0xFFFFFFFF || size > MAXCFG || !ReadFile(h, g_buf, size, &n, NULL) || n != size) {
        CloseHandle(h);
        say("FAIL: C:\\CONFIG.SYS unreadable or over 64 KB");
        goto out;
    }
    CloseHandle(h);

    for (start = 0; start < (int)size; ) {
        int v;
        for (len = 0; start + len < (int)size && g_buf[start + len] != '\r' && g_buf[start + len] != '\n'; len++)
            ;
        if (is_block(g_buf + start, len, block))
            have_block = 1;
        v = key_value(g_buf + start, len, "menudefault");
        if (v >= 0 && vstart < 0) {
            vstart = start + v;
            for (vend = vstart; vend < start + len && g_buf[vend] != ',' && g_buf[vend] != ' '
                               && g_buf[vend] != '\t'; vend++)
                ;
        }
        start += len;
        while (start < (int)size && (g_buf[start] == '\r' || g_buf[start] == '\n')) start++;
    }
    if (vstart < 0) { say("NOT SET: C:\\CONFIG.SYS has no menudefault= line"); rc = 1; goto out; }
    if (!have_block) {
        wsprintfA(line, "NOT SET: C:\\CONFIG.SYS has no [%s] block", block);
        say(line);
        rc = 2;
        goto out;
    }
    blen = lstrlenA(block);
    if (vend - vstart == blen) {
        for (i = 0; i < blen && lower((unsigned char)g_buf[vstart + i]) == lower((unsigned char)block[i]); i++)
            ;
        if (i == blen) {
            wsprintfA(line, "OK: menudefault already names %s - unchanged", block);
            say(line);
            rc = 0;
            goto out;
        }
    }
    /* new = before value + block + after value */
    copy(g_new, g_buf, vstart);
    copy(g_new + vstart, block, blen);
    copy(g_new + vstart + blen, g_buf + vend, (int)size - vend);
    nlen = vstart + blen + (int)(size - vend);

    h = CreateFileA(CFG, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { say("FAIL: C:\\CONFIG.SYS cannot be opened for writing"); rc = 4; goto out; }
    if (SetFilePointer(h, 0, NULL, FILE_BEGIN) != 0 || !WriteFile(h, g_new, nlen, &w, NULL)
            || (int)w != nlen || !SetEndOfFile(h)) {
        /* put the original bytes back before giving up */
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        WriteFile(h, g_buf, size, &w, NULL);
        SetEndOfFile(h);
        CloseHandle(h);
        say("FAIL: write to C:\\CONFIG.SYS failed - original bytes put back");
        rc = 4;
        goto out;
    }
    FlushFileBuffers(h);
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    n = 0;
    if (!ReadFile(h, g_back, nlen, &n, NULL) || (int)n != nlen) n = 0;
    for (i = 0; i < (int)n && g_back[i] == g_new[i]; i++)
        ;
    CloseHandle(h);
    if ((int)n != nlen || i != nlen) { say("FAIL: C:\\CONFIG.SYS did not read back"); rc = 4; goto out; }
    wsprintfA(line, "OK: menudefault now names %s (was %d byte(s) there)", block, vend - vstart);
    say(line);
    rc = 0;
out:
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    ExitProcess(rc);
}
