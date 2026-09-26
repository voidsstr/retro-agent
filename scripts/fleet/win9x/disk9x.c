/* disk9x - READ-ONLY raw access to BIOS hard disks on Windows 95/98, through
 * VWIN32's INT 13h service (VWIN32_DIOC_DOS_INT13 - Microsoft's documented
 * route from a Win32 program to the BIOS disk interface on 9x).
 *
 *   disk9x info                               -> C:\RETRO_AGENT\DISK9X.TXT
 *       for BIOS drives 80h-83h: INT 13h AH=15h (type + sector count),
 *       AH=08h (geometry), AH=41h (extensions present? - advisory, see below)
 *   disk9x read <drive hex> <lba> <count> <outname>
 *       reads <count> sectors (max 128) from <lba> with AH=02h, ONE attempt per
 *       sector, CHS computed from the drive's own AH=08h geometry, into
 *       C:\RETRO_AGENT\<outname> (a bare name; the file must not exist yet).
 *
 * Why it exists (.243, 2026-09-25): a second disk showed in the BIOS but
 * Win98's IDE driver logged "ESDI BIOS read failure" and gave it no drive
 * letter, so nothing in Windows could say what was on it.
 *
 * SAFETY - reviewed adversarially before it first ran (three independent
 * reviews, 2026-09-25); tests/python/test_disk9x_readonly.py pins it:
 *   - Only AH=02h (read), 08h, 15h and 41h are ever issued. No write, format or
 *     extended function, and NO AH=00h RESET: on an AT-class BIOS a reset with
 *     DL>=80h can reset the channel C: sits on while Win98's protected-mode
 *     driver is using it. A failed sector is recorded, never retried.
 *   - The read loop STOPS at the first failed sector, and gives up if any one
 *     BIOS call takes over 3 s or the whole run over 20 s: this disk already
 *     failed a BIOS read at boot, so failure is the expected path, and each
 *     one can cost a full BIOS timeout.
 *   - Every BIOS call is logged BEFORE it is made and again with its time, so
 *     a stall shows exactly where it stopped.
 *   - Output goes only to a NEW file under C:\RETRO_AGENT - a bare name, no
 *     path, never overwriting anything.
 *   - A sector is only "read" if the BIOS cleared carry AND the buffer, pre-
 *     filled with a sentinel, actually changed. An unreadable sector is written
 *     as the text tag DISK9X-UNREADABLE (not 0xEE - that is the GPT
 *     protective-partition type) and named in the log.
 *
 * No C runtime (see README.md): -nostdlib, static buffers. */
#include <windows.h>

/* ZeroMemory / FillMemory are memset; there is no C runtime to supply it */
void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

#define VWIN32_DIOC_DOS_INT13 4
#define OUT_DIR  "C:\\RETRO_AGENT\\"
#define OUT_TXT  "C:\\RETRO_AGENT\\DISK9X.TXT"
#define MAX_READ 128
#define CALL_MAX_MS  3000
#define RUN_MAX_MS  20000
#define SENTINEL 0xA5

typedef struct {
    DWORD reg_EBX, reg_EDX, reg_ECX, reg_EAX, reg_EDI, reg_ESI, reg_Flags;
} DIOC_REGISTERS;

enum { R_OK = 1, R_BIOS_ERR = 0, R_IOCTL_FAIL = -1 };

static HANDLE vw, logf;
static char line[1024];
static unsigned char sector[512];
static DWORD run_start, last_call_ms;

static void w(const char *s)
{
    DWORD n;
    WriteFile(logf, s, lstrlenA(s), &n, NULL);
    WriteFile(logf, "\r\n", 2, &n, NULL);
    FlushFileBuffers(logf);
}

/* One INT 13h call, logged before and after. Returns R_OK (carry clear),
 * R_BIOS_ERR (carry set - the returned AH is the BIOS status) or R_IOCTL_FAIL
 * (VWIN32 refused the call - the registers are NOT a BIOS answer). */
static int int13(DIOC_REGISTERS *r)
{
    DWORD cb = 0, t0;
    BOOL ok;
    char pre[96];
    wsprintfA(pre, "  int13 AH=%02X DL=%02X CX=%04X DH=%02X ...", (unsigned)((r->reg_EAX >> 8) & 0xFF),
              (unsigned)(r->reg_EDX & 0xFF), (unsigned)(r->reg_ECX & 0xFFFF), (unsigned)((r->reg_EDX >> 8) & 0xFF));
    w(pre);
    r->reg_Flags = 1;                       /* preset carry: failure unless cleared */
    t0 = GetTickCount();
    ok = DeviceIoControl(vw, VWIN32_DIOC_DOS_INT13, r, sizeof(*r), r, sizeof(*r), &cb, NULL);
    last_call_ms = GetTickCount() - t0;
    if (!ok) {
        wsprintfA(line, "  ... IOCTL FAILED (GetLastError %lu) after %lu ms", GetLastError(), last_call_ms);
        w(line);
        return R_IOCTL_FAIL;
    }
    wsprintfA(line, "  ... %s AH=%02X AL=%02X after %lu ms", (r->reg_Flags & 1) ? "carry SET" : "ok",
              (unsigned)((r->reg_EAX >> 8) & 0xFF), (unsigned)(r->reg_EAX & 0xFF), last_call_ms);
    w(line);
    return (r->reg_Flags & 1) ? R_BIOS_ERR : R_OK;
}

static int too_slow(void)
{
    return last_call_ms > CALL_MAX_MS || GetTickCount() - run_start > RUN_MAX_MS;
}

static unsigned long hexval(const char *s)
{
    unsigned long v = 0;
    int n = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) {
        char c = *s;
        if (++n > 8) return 0xFFFFFFFFUL;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else return 0xFFFFFFFFUL;
    }
    return n ? v : 0xFFFFFFFFUL;
}

static unsigned long decval(const char *s)
{
    unsigned long v = 0;
    int n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || ++n > 9) return 0xFFFFFFFFUL;
        v = v * 10 + (unsigned long)(*s - '0');
    }
    return n ? v : 0xFFFFFFFFUL;
}

typedef struct { unsigned cyls, heads, spt; int ok; } geo_t;

static geo_t geometry(unsigned drive)
{
    DIOC_REGISTERS r;
    geo_t g = {0, 0, 0, 0};
    ZeroMemory(&r, sizeof(r));
    r.reg_EAX = 0x0800;
    r.reg_EDX = drive;
    if (int13(&r) == R_OK) {
        unsigned ch = (r.reg_ECX >> 8) & 0xFF, cl = r.reg_ECX & 0xFF, dh = (r.reg_EDX >> 8) & 0xFF;
        g.cyls = ((((unsigned)cl & 0xC0) << 2) | ch) + 1;
        g.heads = dh + 1;
        g.spt = cl & 0x3F;
        g.ok = g.spt != 0;
    }
    return g;
}

static void do_info(void)
{
    unsigned d;
    for (d = 0x80; d <= 0x83; d++) {
        DIOC_REGISTERS r;
        geo_t g;
        int rc;
        ZeroMemory(&r, sizeof(r));
        r.reg_EAX = 0x1500;
        r.reg_EDX = d;
        rc = int13(&r);
        if (rc == R_OK) {
            unsigned type = (r.reg_EAX >> 8) & 0xFF;
            if (type == 3)
                wsprintfA(line, "drive %02X: AH=15h type=3 (fixed disk) sectors=%lu", d,
                          (unsigned long)(((r.reg_ECX & 0xFFFF) << 16) | (r.reg_EDX & 0xFFFF)));
            else
                wsprintfA(line, "drive %02X: AH=15h type=%u (not a fixed disk; 0 = no drive)", d, type);
        } else if (rc == R_BIOS_ERR) {
            wsprintfA(line, "drive %02X: AH=15h BIOS error, status %02X", d, (unsigned)((r.reg_EAX >> 8) & 0xFF));
        } else {
            wsprintfA(line, "drive %02X: AH=15h not answered (VWIN32 refused the call)", d);
        }
        w(line);
        g = geometry(d);
        if (g.ok)
            wsprintfA(line, "drive %02X: AH=08h C/H/S = %u/%u/%u -> %lu sectors (%lu MB) addressable by CHS",
                      d, g.cyls, g.heads, g.spt,
                      (unsigned long)g.cyls * g.heads * g.spt,
                      (unsigned long)g.cyls * g.heads * g.spt / 2048UL);
        else
            wsprintfA(line, "drive %02X: AH=08h gave no geometry", d);
        w(line);
        /* AH=41h passes 55AAh in BX; VWIN32 may treat EBX as a buffer pointer and
         * translate it, so a negative answer here is advisory, not proof. */
        ZeroMemory(&r, sizeof(r));
        r.reg_EAX = 0x4100;
        r.reg_EBX = 0x55AA;
        r.reg_EDX = d;
        if (int13(&r) == R_OK && (r.reg_EBX & 0xFFFF) == 0xAA55)
            wsprintfA(line, "drive %02X: AH=41h extensions present, version %02X, support bits %04X",
                      d, (unsigned)((r.reg_EAX >> 8) & 0xFF), (unsigned)(r.reg_ECX & 0xFFFF));
        else
            wsprintfA(line, "drive %02X: AH=41h extensions not detected via VWIN32 (advisory)", d);
        w(line);
        if (too_slow()) { w("info: BIOS too slow - stopping"); return; }
    }
}

/* 1 = read, 0 = failed (why in *why). One attempt: no reset, no retry. */
static int read_sector(unsigned drive, const geo_t *g, unsigned long lba, char *why)
{
    unsigned long per_cyl = (unsigned long)g->heads * g->spt;
    unsigned cyl = (unsigned)(lba / per_cyl);
    unsigned head = (unsigned)((lba % per_cyl) / g->spt);
    unsigned sec = (unsigned)(lba % g->spt) + 1;
    DIOC_REGISTERS r;
    int rc, i, changed = 0;
    if (cyl >= g->cyls) { lstrcpyA(why, "beyond the CHS geometry - not readable this way"); return 0; }
    FillMemory(sector, sizeof(sector), SENTINEL);
    ZeroMemory(&r, sizeof(r));
    r.reg_EAX = 0x0201;                                     /* AH=02h read, AL=1 sector */
    r.reg_ECX = ((cyl & 0xFF) << 8) | ((cyl >> 2) & 0xC0) | (sec & 0x3F);
    r.reg_EDX = (head << 8) | drive;
    r.reg_EBX = (DWORD)sector;
    rc = int13(&r);
    if (rc == R_IOCTL_FAIL) { lstrcpyA(why, "VWIN32 refused the call"); return 0; }
    if (rc == R_BIOS_ERR) {
        wsprintfA(why, "BIOS status %02X", (unsigned)((r.reg_EAX >> 8) & 0xFF));
        return 0;
    }
    for (i = 0; i < (int)sizeof(sector); i++)
        if (sector[i] != SENTINEL) { changed = 1; break; }
    if (!changed) { lstrcpyA(why, "carry clear but no data transferred"); return 0; }
    return 1;
}

static int bare_name(const char *s)
{
    int n = 0;
    for (; *s; s++, n++)
        if (*s == ':' || *s == '\\' || *s == '/' || n > 60) return 0;
    return n > 0;
}

static void do_read(const char *sdrive, const char *slba, const char *scount, const char *outname)
{
    unsigned long drive = hexval(sdrive), lba = decval(slba), count = decval(scount), i, good = 0;
    geo_t g;
    HANDLE of;
    DWORD n;
    char path[128], why[64];
    if (drive < 0x80 || drive > 0x83 || lba == 0xFFFFFFFFUL || count == 0 || count > MAX_READ
            || !bare_name(outname)) {
        w("read: bad arguments (drive 80-83 hex, lba decimal, count 1-128, a bare output file name)");
        return;
    }
    wsprintfA(path, "%s%s", OUT_DIR, outname);
    g = geometry((unsigned)drive);
    if (!g.ok) { wsprintfA(line, "read %02lX: no geometry (AH=08h failed) - nothing read", drive); w(line); return; }
    of = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (of == INVALID_HANDLE_VALUE) { wsprintfA(line, "read: cannot create %s (exists already?)", path); w(line); return; }
    for (i = 0; i < count; i++) {
        if (!read_sector((unsigned)drive, &g, lba + i, why)) {
            static const char tag[] = "DISK9X-UNREADABLE";
            ZeroMemory(sector, sizeof(sector));
            CopyMemory(sector, tag, sizeof(tag) - 1);
            WriteFile(of, sector, 512, &n, NULL);
            wsprintfA(line, "read %02lX lba %lu: FAILED (%s) - stopping at the first failure", drive, lba + i, why);
            w(line);
            break;
        }
        WriteFile(of, sector, 512, &n, NULL);
        good++;
        if (too_slow()) {
            wsprintfA(line, "read %02lX: BIOS too slow (%lu ms for one call) - stopping", drive, last_call_ms);
            w(line);
            break;
        }
    }
    CloseHandle(of);
    wsprintfA(line, "read %02lX lba %lu asked %lu -> %s : %lu sector(s) read", drive, lba, count, path, good);
    w(line);
}

/* split the command line into up to 8 whitespace-separated tokens */
static int split(char *c, char **tok, int max)
{
    int n = 0;
    while (*c && n < max) {
        while (*c == ' ' || *c == '\t') c++;
        if (!*c) break;
        if (*c == '"') {
            tok[n++] = ++c;
            while (*c && *c != '"') c++;
        } else {
            tok[n++] = c;
            while (*c && *c != ' ' && *c != '\t') c++;
        }
        if (*c) *c++ = 0;
    }
    return n;
}

static char cmdbuf[512];

void __stdcall start(void)
{
    char *tok[8];
    int n;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    run_start = GetTickCount();
    CreateDirectoryA("C:\\RETRO_AGENT", NULL);
    logf = CreateFileA(OUT_TXT, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (logf == INVALID_HANDLE_VALUE) ExitProcess(3);
    SetFilePointer(logf, 0, NULL, FILE_END);
    wsprintfA(line, "--begin-- tick %lu: %.300s", run_start, GetCommandLineA());
    w(line);
    lstrcpynA(cmdbuf, GetCommandLineA(), sizeof(cmdbuf));
    n = split(cmdbuf, tok, 8);
    vw = CreateFileA("\\\\.\\vwin32", 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (vw == INVALID_HANDLE_VALUE) { w("cannot open \\\\.\\vwin32 (not Windows 9x?)"); w("--end--"); ExitProcess(4); }
    if (n >= 2 && !lstrcmpiA(tok[1], "info"))
        do_info();
    else if (n >= 6 && !lstrcmpiA(tok[1], "read"))
        do_read(tok[2], tok[3], tok[4], tok[5]);
    else
        w("usage: disk9x info | disk9x read <drive hex 80-83> <lba> <count 1-128> <bare output name>");
    w("--end--");
    CloseHandle(vw);
    CloseHandle(logf);
    ExitProcess(0);
}
