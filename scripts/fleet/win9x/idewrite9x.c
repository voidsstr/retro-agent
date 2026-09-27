/* idewrite9x - WRITE sectors to the disk on the SECONDARY IDE master of a
 * Windows 95/98 box, through the controller's legacy ports, bypassing the BIOS.
 * The deliberately separate, write-capable sibling of ide9x (which stays
 * read-only). Every command names the target disk by its SERIAL NUMBER.
 *
 *   idewrite9x zero  <serial> <lba> <count>   write <count> zero sectors (<= 262144)
 *   idewrite9x put   <serial> <lba> <name>    write C:\RETRO_AGENT\<name> (whole
 *                                             sectors, <= 64 KB) starting at <lba>
 *   idewrite9x flush <serial>                 ATA FLUSH CACHE
 *   idewrite9x hpa <serial> <maxlba>          ATA SET MAX ADDRESS (a Host Protected Area)
 *   log: C:\RETRO_AGENT\IDEW9X.TXT
 *
 * Why it exists (.243, 2026-09-25): the operator asked for the second disk - an
 * 80 GB Seagate ST380013A the 1997 BIOS cannot address and Win98 cannot mount -
 * to be wiped and formatted for Win98. No Windows or BIOS tool can reach it, so
 * the FAT32 layout is built on the host with mkfs.fat (checked with fsck.fat)
 * and written here, then read back with ide9x and compared byte for byte.
 *
 * SAFETY (adversarially reviewed before first use; pinned by
 * tests/python/test_idewrite9x_guards.py):
 *   - SECONDARY MASTER ONLY. Ports are the constants 170h-177h/376h; the
 *     primary channel (C: and the DVD, under Win98's own driver) is never
 *     addressed, and the device/head register is only ever given the MASTER bit.
 *   - THE SERIAL MUST MATCH. Before anything is written, IDENTIFY is run and the
 *     drive's serial must equal the <serial> argument exactly; otherwise nothing
 *     is written. A swapped cable or a different disk cannot be written by
 *     mistake.
 *   - Writes stay inside the drive's LBA28 capacity from that same IDENTIFY.
 *   - Only ATA commands ECh (IDENTIFY), 30h (WRITE SECTORS) and E7h (FLUSH CACHE).
 *     Device control only ever 02h (nIEN) or 08h - never SRST.
 *   - Polled, interrupts off while busy, a timeout on every wait, and it stops
 *     at the first error, logging status and error registers.
 *   - NEVER kill it mid-run (a drive left in a data-out phase wedges the channel
 *     until a reset). To stop a zero run, create C:\RETRO_AGENT\IDEW9X.STP; it is
 *     checked between 256-sector runs. Progress is logged every 8192 sectors.
 *   - A named mutex shared with ide9x keeps the two tools off the channel at
 *     the same time.
 *
 * No C runtime (see README.md): -nostdlib, static buffers. */
#include <windows.h>

void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

#define IDE_BASE   0x170          /* never 0x1F0 */
#define IDE_CTRL   0x376
#define R_DATA     (IDE_BASE + 0)
#define R_ERROR    (IDE_BASE + 1)
#define R_COUNT    (IDE_BASE + 2)
#define R_LBA0     (IDE_BASE + 3)
#define R_LBA1     (IDE_BASE + 4)
#define R_LBA2     (IDE_BASE + 5)
#define R_DEVHEAD  (IDE_BASE + 6)
#define R_CMDSTAT  (IDE_BASE + 7)

#define ATA_IDENTIFY       0xEC
#define ATA_WRITE_SECTORS  0x30
#define ATA_FLUSH_CACHE    0xE7
#define ATA_READ_NATIVE_MAX 0xF8  /* HPA: read the drive's native max LBA      */
#define ATA_SET_MAX        0xF9   /* HPA: set the max LBA the drive reports    */
#define R_FEATURES (IDE_BASE + 1)  /* write side of the error register           */
#define HPA_VV     0x01           /* SET MAX sector count bit 0: keep across power cycles */

#define CTL_NIEN   0x02
#define CTL_IDLE   0x08
#define DEV_MASTER_LBA 0xE0       /* master, LBA - the only device ever selected */

#define ST_BSY  0x80
#define ST_DF   0x20
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define OUT_DIR  "C:\\RETRO_AGENT\\"
#define OUT_TXT  "C:\\RETRO_AGENT\\IDEW9X.TXT"
#define STOP_FILE "C:\\RETRO_AGENT\\IDEW9X.STP"   /* create it to stop a zero run cleanly */
#define SEL_MS   3000
#define CMD_MS  31000
#define MAX_ZERO 262144UL         /* 128 MB per run */
#define MAX_PUT  128              /* sectors (64 KB) */

static HANDLE logf;
static char line[1024];
static unsigned short ident[256];
static unsigned short zero_buf[256];
static unsigned char put_buf[MAX_PUT * 512];
static unsigned long capacity;

static void w(const char *s)
{
    DWORD n;
    WriteFile(logf, s, lstrlenA(s), &n, NULL);
    WriteFile(logf, "\r\n", 2, &n, NULL);
    FlushFileBuffers(logf);
}

static unsigned char inb(unsigned short port)
{
    unsigned char v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static unsigned short inw(unsigned short port)
{
    unsigned short v;
    __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void outb(unsigned short port, unsigned char v)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
static void outw(unsigned short port, unsigned short v)
{
    __asm__ __volatile__("outw %0, %1" : : "a"(v), "Nd"(port));
}



static int lstrcmpiA_prefix(const char *s, const char *prefix)
{
    char a, b;
    for (; *prefix; s++, prefix++) {
        a = *s; b = *prefix;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 1;
    }
    return 0;
}
static int contains_ci(const char *s, const char *needle)
{
    for (; *s; s++) if (!lstrcmpiA_prefix(s, needle)) return 1;
    return 0;
}

/* Is the secondary channel live under WINDOWS? Then no raw port access, ever.
 * On .243 (2026-09-27) an IDENTIFY sent while Win98's ESDI_506 owned the
 * channel (after the disk came online natively) left the driver waiting on an
 * interrupt the tool had masked; the agent's next query of D: blocked and the
 * agent died, with nobody at the box. Reads HKEY_DYN_DATA (the live devnode
 * tree): refuses when MF\CHILD0001 has no problem code (the channel started),
 * when any devnode hangs off &CHILD0001& (a disk Windows claimed there), or
 * when the tree cannot be read at all (fail closed). */
static int channel_owned_by_windows(char *why, int whylen)
{
    HKEY root, k;
    char name[64], hw[256];
    DWORD i, n, t, sz, problem;
    if (RegOpenKeyExA((HKEY)(ULONG_PTR)0x80000006UL, "Config Manager\\Enum", 0, KEY_READ, &root) != ERROR_SUCCESS) {
        lstrcpynA(why, "cannot read HKEY_DYN_DATA\\Config Manager\\Enum (not Win9x?)", whylen);
        return 1;
    }
    for (i = 0;; i++) {
        n = sizeof(name);
        if (RegEnumKeyExA(root, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExA(root, name, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        sz = sizeof(hw);
        if (RegQueryValueExA(k, "HardWareKey", NULL, &t, (BYTE *)hw, &sz) == ERROR_SUCCESS) {
            hw[sizeof(hw) - 1] = 0;
            problem = 0xFFFFFFFFUL; sz = sizeof(problem);
            RegQueryValueExA(k, "Problem", NULL, &t, (BYTE *)&problem, &sz);
            if (!lstrcmpiA_prefix(hw, "MF\\CHILD0001\\") && problem == 0) {
                lstrcpynA(why, "the secondary IDE channel (MF\\CHILD0001) is running under Windows", whylen);
                RegCloseKey(k); RegCloseKey(root); return 1;
            }
            if (contains_ci(hw, "&CHILD0001&")) {
                wsprintfA(why, "Windows has a device on the secondary channel: %.120s", hw);
                RegCloseKey(k); RegCloseKey(root); return 1;
            }
        }
        RegCloseKey(k);
    }
    RegCloseKey(root);
    return 0;
}

/* One process at a time on the secondary channel: ide9x and idewrite9x both
 * take this named mutex and exit if the other holds it (their task-file
 * writes must never interleave). */
static HANDLE chan_lock(void)
{
    HANDLE m = CreateMutexA(NULL, TRUE, "retro_ide_secondary");
    if (!m) return NULL;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(m); return NULL; }
    return m;
}

/* the 400 ns settle the ATA spec asks for (alternate status, 4 reads) */
static void settle(void) { inb(IDE_CTRL); inb(IDE_CTRL); inb(IDE_CTRL); inb(IDE_CTRL); }

static int wait_notbusy(DWORD ms)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        unsigned char s = 0xFF;
        int spin;
        /* spin briefly first: a drive is BSY for microseconds after a block,
         * and Sleep(1) on 9x costs a whole scheduler tick per sector */
        for (spin = 0; spin < 2000; spin++) {
            s = inb(R_CMDSTAT);
            if (s != 0xFF && !(s & ST_BSY)) return s;
        }
        if (GetTickCount() - t0 > ms) return -1;
        Sleep(1);
    }
}

/* select the master (LBA mode), interrupts off, and require it idle */
static int select_master(unsigned char lba_top)
{
    int s;
    outb(IDE_CTRL, CTL_NIEN);
    outb(R_DEVHEAD, (unsigned char)(DEV_MASTER_LBA | (lba_top & 0x0F)));
    settle();
    s = wait_notbusy(SEL_MS);
    if (s < 0 || s == 0xFF || (s & (ST_BSY | ST_DRQ))) return -1;
    return s;
}

static int wait_drq(const char *what)
{
    int s = wait_notbusy(CMD_MS);
    if (s < 0) { wsprintfA(line, "%s: TIMEOUT, still BSY", what); w(line); return -1; }
    if (s & (ST_ERR | ST_DF)) {
        wsprintfA(line, "%s: error - status %02X error %02X", what, s, inb(R_ERROR)); w(line); return -1;
    }
    if (!(s & ST_DRQ)) { wsprintfA(line, "%s: no DRQ (status %02X)", what, s); w(line); return -1; }
    return s;
}

static int wait_done(const char *what)
{
    int s = wait_notbusy(CMD_MS);
    if (s < 0) { wsprintfA(line, "%s: TIMEOUT waiting for completion", what); w(line); return -1; }
    if (s & (ST_ERR | ST_DF | ST_DRQ)) {
        wsprintfA(line, "%s: did not complete cleanly - status %02X error %02X", what, s, inb(R_ERROR));
        w(line);
        return -1;
    }
    return s;
}

/* exact byte comparison - lstrcmpA is a locale compare that ignores some characters */
static int bytes_equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void trimcopy(const unsigned short *wds, int from, int n, char *out)
{
    int i, j = 0, k = 0;
    char tmp[64];
    for (i = from; i < from + n; i++) { tmp[j++] = (char)(wds[i] >> 8); tmp[j++] = (char)(wds[i] & 0xFF); }
    tmp[j] = 0;
    for (i = 0; tmp[i] == ' '; i++) ;
    for (; tmp[i]; i++) out[k++] = tmp[i];
    out[k] = 0;
    while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == 0)) out[--k] = 0;
}

/* IDENTIFY the master and require its serial to be exactly <want>. */
static int verify_target(const char *want)
{
    int i;
    char serial[32], model[48];
    if (select_master(0) < 0) { w("target: master not idle - nothing written"); return 0; }
    outb(R_CMDSTAT, ATA_IDENTIFY);
    settle();
    if (wait_drq("IDENTIFY") < 0) { w("target: IDENTIFY failed - nothing written"); return 0; }
    for (i = 0; i < 256; i++) ident[i] = inw(R_DATA);
    trimcopy(ident, 10, 10, serial);
    trimcopy(ident, 27, 20, model);
    capacity = ((unsigned long)ident[61] << 16) | ident[60];
    if (capacity > 0x10000000UL) capacity = 0x10000000UL;    /* LBA28 addressing limit */
    wsprintfA(line, "target: secondary master MODEL \"%s\" SERIAL \"%s\" LBA28 sectors %lu", model, serial, capacity);
    w(line);
    if (!bytes_equal(serial, want)) {
        wsprintfA(line, "target: SERIAL MISMATCH - asked for \"%s\" - nothing written", want);
        w(line);
        return 0;
    }
    if (!((ident[49] >> 9) & 1) || capacity == 0) { w("target: no LBA support - nothing written"); return 0; }
    return 1;
}

/* WRITE SECTORS: up to 256 sectors from src (a sector index advances only
 * when src_step is nonzero; zero writes reuse one zero buffer). */
static int write_run(unsigned long lba, unsigned count, const unsigned short *src, int src_step)
{
    unsigned i;
    int k;
    char what[64];
    if (count == 0 || count > 256) return -1;
    if (lba >= capacity || count > capacity - lba) {
        wsprintfA(line, "write lba %lu count %u: beyond the drive (capacity %lu) - refused", lba, count, capacity);
        w(line);
        return -1;
    }
    if (select_master((unsigned char)((lba >> 24) & 0x0F)) < 0) { w("write: master not idle - stopping"); return -1; }
    outb(R_COUNT, (unsigned char)(count == 256 ? 0 : count));
    outb(R_LBA0, (unsigned char)(lba & 0xFF));
    outb(R_LBA1, (unsigned char)((lba >> 8) & 0xFF));
    outb(R_LBA2, (unsigned char)((lba >> 16) & 0xFF));
    outb(R_CMDSTAT, ATA_WRITE_SECTORS);
    settle();
    for (i = 0; i < count; i++) {
        const unsigned short *p = src + (src_step ? (unsigned long)i * 256 : 0);
        wsprintfA(what, "WRITE lba %lu", lba + i);
        if (wait_drq(what) < 0) return -1;
        for (k = 0; k < 256; k++) outw(R_DATA, p[k]);
        settle();
    }
    wsprintfA(what, "WRITE lba %lu+%u", lba, count);
    if (wait_done(what) < 0) return -1;
    return 0;
}

static int parse_dec(const char *s, unsigned long *out)
{
    unsigned long v = 0;
    int n = 0;
    for (; *s; s++, n++) {
        if (*s < '0' || *s > '9' || n >= 9) return 0;
        v = v * 10 + (unsigned long)(*s - '0');
    }
    *out = v;
    return n > 0;
}

static int safe_name(const char *s)
{
    int i;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
              || c == '_' || c == '.' || c == '-') || i >= 60)
            return 0;
    }
    return i > 0;
}

static void do_zero(const char *serial, const char *slba, const char *scount)
{
    unsigned long lba, count, done = 0;
    DWORD t0 = GetTickCount();
    if (!parse_dec(slba, &lba) || !parse_dec(scount, &count) || count == 0 || count > MAX_ZERO) {
        w("zero: bad arguments (lba, count 1-262144)");
        return;
    }
    if (!verify_target(serial)) return;
    while (done < count) {
        unsigned n = (unsigned)((count - done) > 256 ? 256 : (count - done));
        if (GetFileAttributesA(STOP_FILE) != 0xFFFFFFFFUL) {    /* between runs only, never mid-run */
            wsprintfA(line, "zero: stop file %s present - stopping cleanly after %lu sector(s)", STOP_FILE, done);
            w(line);
            break;
        }
        if (write_run(lba + done, n, zero_buf, 0) < 0) break;
        done += n;
        if (done % 8192 == 0) {
            wsprintfA(line, "zero: %lu/%lu sector(s), %lu ms", done, count, GetTickCount() - t0);
            w(line);
        }
    }
    wsprintfA(line, "zero lba %lu count %lu: %lu sector(s) written in %lu ms%s", lba, count, done,
              GetTickCount() - t0, done == count ? "" : " - INCOMPLETE");
    w(line);
}

static void do_put(const char *serial, const char *slba, const char *name)
{
    unsigned long lba;
    char path[128];
    HANDLE f;
    DWORD got = 0, size;
    if (!parse_dec(slba, &lba) || !safe_name(name)) { w("put: bad arguments (lba, bare file name)"); return; }
    wsprintfA(path, "%s%s", OUT_DIR, name);
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { wsprintfA(line, "put: cannot open %s", path); w(line); return; }
    size = GetFileSize(f, NULL);
    if (size == 0 || size % 512 || size > sizeof(put_buf)) {
        CloseHandle(f);
        wsprintfA(line, "put: %s is %lu bytes - must be whole sectors, at most %d", path, size, MAX_PUT * 512);
        w(line);
        return;
    }
    if (!ReadFile(f, put_buf, size, &got, NULL) || got != size) { CloseHandle(f); w("put: short read of the file"); return; }
    CloseHandle(f);
    if (!verify_target(serial)) return;
    if (write_run(lba, (unsigned)(size / 512), (const unsigned short *)put_buf, 1) == 0)
        wsprintfA(line, "put %s -> lba %lu: %lu sector(s) written", name, lba, size / 512);
    else
        wsprintfA(line, "put %s -> lba %lu: FAILED", name, lba);
    w(line);
}

/* READ NATIVE MAX ADDRESS: the drive's real last LBA (unaffected by any HPA).
 * Returns -1 on failure. */
static long read_native_max(unsigned long *out)
{
    unsigned long lba;
    if (select_master(0) < 0) { w("hpa: master not idle"); return -1; }
    outb(R_CMDSTAT, ATA_READ_NATIVE_MAX);
    settle();
    if (wait_done("READ NATIVE MAX") < 0) return -1;
    lba = ((unsigned long)(inb(R_DEVHEAD) & 0x0F) << 24) | ((unsigned long)inb(R_LBA2) << 16) |
          ((unsigned long)inb(R_LBA1) << 8) | inb(R_LBA0);
    *out = lba;
    return 0;
}

/*
 * hpa <serial> <maxlba>: make the drive REPORT maxlba+1 sectors (a Host
 * Protected Area), kept across power cycles. Why: .243's 1997 Compaq BIOS
 * translates by doubling heads while cylinders > 1024, so the 80 GB drive's
 * 16383 default cylinders overflow to 256 heads and every BIOS read above
 * head 0 fails. At 8191 cylinders (maxlba 8,256,527) it translates to
 * 1023/128/63, which BIOS, real DOS and Windows all handle. Reversible: run
 * it again with maxlba = the native max it logs.
 * Guards: exact serial first; the target must be the native max (undo) or a
 * whole number of 16x63 cylinders no larger than 16383 of them; SET MAX is
 * issued immediately after READ NATIVE MAX, as the ATA spec requires; the
 * result is read back through IDENTIFY and reported, never assumed.
 */
static void do_hpa(const char *serial, const char *smax)
{
    unsigned long maxlba, native, want_sectors, cyl;
    if (!verify_target(serial)) return;
    if (!parse_dec(smax, &maxlba)) { w("hpa: bad max LBA - nothing written"); return; }
    if (read_native_max(&native) < 0) { w("hpa: READ NATIVE MAX failed - nothing written"); return; }
    wsprintfA(line, "hpa: native max LBA %lu (%lu sectors); drive currently reports %lu sectors",
              native, native + 1, capacity);
    w(line);
    want_sectors = maxlba + 1;
    cyl = want_sectors / (16UL * 63UL);
    if (maxlba > native) { w("hpa: target beyond the native max - refused, nothing written"); return; }
    if (maxlba != native && (want_sectors % (16UL * 63UL) != 0 || cyl == 0 || cyl > 16383)) {
        w("hpa: target is neither the native max nor a whole number of 16x63 cylinders (<= 16383) - refused");
        return;
    }
    if (want_sectors == capacity) { w("hpa: the drive already reports that size - nothing written"); return; }
    /* SET MAX must directly follow READ NATIVE MAX: issue it again right here. */
    if (read_native_max(&native) < 0) { w("hpa: READ NATIVE MAX (2) failed - nothing written"); return; }
    outb(R_FEATURES, 0x00);                         /* 00 = SET MAX ADDRESS itself */
    outb(R_COUNT, HPA_VV);
    outb(R_LBA0, (unsigned char)(maxlba & 0xFF));
    outb(R_LBA1, (unsigned char)((maxlba >> 8) & 0xFF));
    outb(R_LBA2, (unsigned char)((maxlba >> 16) & 0xFF));
    outb(R_DEVHEAD, (unsigned char)(DEV_MASTER_LBA | ((maxlba >> 24) & 0x0F)));
    outb(R_CMDSTAT, ATA_SET_MAX);
    settle();
    if (wait_done("SET MAX ADDRESS") < 0) { w("hpa: SET MAX ADDRESS failed - check IDENTIFY"); }
    /* the post-condition, from the drive itself */
    if (!verify_target(serial)) { w("hpa: IDENTIFY after SET MAX failed"); return; }
    wsprintfA(line, "hpa: drive now reports %lu sectors, default CHS %u/%u/%u (%s)",
              capacity, ident[1], ident[3], ident[6],
              capacity == want_sectors ? "as requested" : "NOT what was requested");
    w(line);
}

static void do_flush(const char *serial)
{
    if (!verify_target(serial)) return;
    if (select_master(0) < 0) { w("flush: master not idle"); return; }
    outb(R_CMDSTAT, ATA_FLUSH_CACHE);
    settle();
    if (wait_done("FLUSH CACHE") >= 0) w("flush: done");    /* wait_done returns the status byte, not 0 */
}

static void restore(void)
{
    int s = inb(R_CMDSTAT);
    if (s != 0xFF && !(s & (ST_BSY | ST_DRQ))) {
        outb(IDE_CTRL, CTL_IDLE);
        wsprintfA(line, "exit: device idle (status %02X) - interrupts re-enabled", s);
    } else {
        wsprintfA(line, "exit: status %02X - leaving interrupts disabled", s);
    }
    w(line);
}

static int split(char *c, char **tok, int max)
{
    int n = 0;
    while (*c && n < max) {
        while (*c == ' ' || *c == '\t') c++;
        if (!*c) break;
        if (*c == '"') { tok[n++] = ++c; while (*c && *c != '"') c++; }
        else { tok[n++] = c; while (*c && *c != ' ' && *c != '\t') c++; }
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
    if (!(GetVersion() & 0x80000000)) ExitProcess(5);   /* 9x only */
    CreateDirectoryA("C:\\RETRO_AGENT", NULL);
    logf = CreateFileA(OUT_TXT, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (logf == INVALID_HANDLE_VALUE) ExitProcess(3);
    SetFilePointer(logf, 0, NULL, FILE_END);
    wsprintfA(line, "--begin-- tick %lu: %.300s", GetTickCount(), GetCommandLineA());
    w(line);
    if (!chan_lock()) { w("another ide9x/idewrite9x is using the secondary channel - exiting"); w("--end--"); ExitProcess(6); }
    {
        char why[200];
        if (channel_owned_by_windows(why, sizeof(why))) {
            w("REFUSED - no port access while Windows owns the channel:"); w(why); w("--end--"); ExitProcess(7);
        }
    }
    lstrcpynA(cmdbuf, GetCommandLineA(), sizeof(cmdbuf));
    n = split(cmdbuf, tok, 8);
    if (n >= 5 && !lstrcmpiA(tok[1], "zero"))
        { do_zero(tok[2], tok[3], tok[4]); restore(); }
    else if (n >= 5 && !lstrcmpiA(tok[1], "put"))
        { do_put(tok[2], tok[3], tok[4]); restore(); }
    else if (n >= 4 && !lstrcmpiA(tok[1], "hpa"))
        { do_hpa(tok[2], tok[3]); restore(); }
    else if (n >= 3 && !lstrcmpiA(tok[1], "flush"))
        { do_flush(tok[2]); restore(); }
    else
        w("usage: idewrite9x zero <serial> <lba> <count> | put <serial> <lba> <name> | flush <serial> | hpa <serial> <maxlba>");
    w("--end--");
    CloseHandle(logf);
    ExitProcess(0);
}
