/* ide9x - READ-ONLY identification and sector dump of a disk on the SECONDARY
 * IDE channel of a Windows 95/98 box, by talking to the controller's legacy
 * ports directly (170h-177h, 376h), bypassing the BIOS.
 *
 *   ide9x identify                    -> C:\RETRO_AGENT\IDE9X.TXT
 *       PIIX IDE timing for the secondary channel (PCI 00:07.1 reg 40h-43h,
 *       read-only), a register read-back presence test, then for master and
 *       slave: ATA IDENTIFY DEVICE (ECh) - model, serial, firmware, LBA and
 *       capacity - and the raw block in C:\RETRO_AGENT\IDENT_M.BIN / IDENT_S.BIN
 *   ide9x read <m|s> <lba> <count> <outname>
 *       IDENTIFY first (to prove LBA support), then ATA READ SECTORS (20h), LBA28,
 *       up to 128 sectors, PIO, polled, into a NEW file C:\RETRO_AGENT\<outname>
 *
 * Why it exists (.243, 2026-09-25): a second disk showed in the BIOS, Win98's
 * IDE driver logged "ESDI BIOS read failure" and gave it no drive letter,
 * VWIN32's INT 13h service does not serve hard disks on 9x at all, and FDISK
 * /STATUS in a DOS box stalled the machine for five minutes probing it through
 * the BIOS. The disk is on the channel Windows found NO device on (C: is primary
 * master, the DVD writer primary slave), so talking to that channel's ports is
 * the one route that involves neither the BIOS nor a Windows driver.
 *
 * SAFETY (two adversarial reviews of three lenses each before first use;
 * pinned by tests/python/test_ide9x_readonly.py):
 *   - SECONDARY CHANNEL ONLY: every port is a constant in 170h-177h / 376h. The
 *     primary channel (1F0h-1F7h, 3F6h), where C: and the DVD live under Win98's
 *     own driver, is never addressed. PCI config is only READ (CF8h/CFCh).
 *   - Only two ATA commands exist here: ECh IDENTIFY DEVICE and 20h READ SECTORS.
 *     376h is only ever written with 02h (nIEN) or 08h (the conventional idle
 *     value) - never SRST (04h), so the channel is never reset.
 *   - Interrupts are off (nIEN) while anything is in flight and every wait is
 *     polled with a timeout. nIEN is restored at exit ONLY if the drive is idle.
 *   - A device is only commanded with BSY=0 and DRQ=0. A stale data block from a
 *     timed-out command is drained and logged, never read as a later sector.
 *   - READ refuses a drive that does not report LBA support (word 49 bit 9).
 *   - Output only as NEW files under C:\RETRO_AGENT, names [A-Za-z0-9_.-] and
 *     never a DOS device name (LPT1, COM1, CON ... open the device on 9x).
 *
 * No C runtime (see README.md): -nostdlib, static buffers. */
#include <windows.h>

void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

/* ---- the ONLY IDE ports this program ever touches: the secondary channel ---- */
#define IDE_BASE   0x170          /* never 0x1F0 - see the header */
#define IDE_CTRL   0x376
#define R_DATA     (IDE_BASE + 0)
#define R_ERROR    (IDE_BASE + 1)
#define R_COUNT    (IDE_BASE + 2)
#define R_LBA0     (IDE_BASE + 3)
#define R_LBA1     (IDE_BASE + 4)
#define R_LBA2     (IDE_BASE + 5)
#define R_DEVHEAD  (IDE_BASE + 6)
#define R_CMDSTAT  (IDE_BASE + 7)
/* PCI configuration mechanism #1 - read-only use */
#define PCI_ADDR   0xCF8
#define PCI_DATA   0xCFC

/* ---- the ONLY commands this program can issue ---- */
#define ATA_IDENTIFY      0xEC
#define ATA_READ_SECTORS  0x20
/* ---- the ONLY values written to the device-control register ---- */
#define CTL_NIEN   0x02
#define CTL_IDLE   0x08

#define ST_BSY  0x80
#define ST_DF   0x20
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define OUT_DIR  "C:\\RETRO_AGENT\\"
#define OUT_TXT  "C:\\RETRO_AGENT\\IDE9X.TXT"
#define MAX_READ 128
#define SEL_MS   3000             /* device select / idle waits */
#define CMD_MS  31000             /* after a command: spin-up + internal retries */

static HANDLE logf;
static char line[1024];
static unsigned short buf[256];
static unsigned short ident[256];

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
static unsigned long inl(unsigned short port)
{
    unsigned long v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void outb(unsigned short port, unsigned char v)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
static void outl(unsigned short port, unsigned long v)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}

/* the 400 ns settle the ATA spec asks for (alternate status, 4 reads) */
static void settle(void) { inb(IDE_CTRL); inb(IDE_CTRL); inb(IDE_CTRL); inb(IDE_CTRL); }

/* Poll the status register. Returns the status once BSY=0 (whatever else is
 * set - the caller judges DRQ/ERR/DF), or -1 on timeout (still BSY or 0xFF). */
static int wait_notbusy(DWORD ms)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        unsigned char s = inb(R_CMDSTAT);
        if (s != 0xFF && !(s & ST_BSY)) return s;
        if (GetTickCount() - t0 > ms) return -1;
        Sleep(1);
    }
}

/* Drain a data block a device is still offering (a command that finished after
 * we gave up on it). Up to 256 blocks; each is logged. Returns final status. */
static int drain(const char *who)
{
    int blocks = 0, s = inb(R_CMDSTAT);
    while (s != 0xFF && !(s & ST_BSY) && (s & ST_DRQ) && blocks < 256) {
        int k;
        for (k = 0; k < 256; k++) inw(R_DATA);
        blocks++;
        settle();
        s = inb(R_CMDSTAT);
    }
    if (blocks) {
        wsprintfA(line, "%s: drained %d stale data block(s) left by an earlier command", who, blocks);
        w(line);
    }
    return s;
}

/* Select master (0) or slave (1), LBA bit set, interrupts off; then require the
 * NEWLY selected device to be idle (BSY=0, DRQ=0). Returns status or -1. */
static int select_dev(int slave, unsigned char lba_top, const char *who)
{
    int s;
    outb(IDE_CTRL, CTL_NIEN);
    outb(R_DEVHEAD, (unsigned char)(0xE0 | (slave ? 0x10 : 0) | (lba_top & 0x0F)));
    settle();
    s = wait_notbusy(SEL_MS);
    if (s < 0) return -1;
    if (s & ST_DRQ) s = drain(who);
    if (s == 0xFF || (s & (ST_BSY | ST_DRQ))) return -1;
    return s;
}

/* After a command: wait up to CMD_MS for BSY to clear, then judge. */
static int wait_data(const char *who, const char *what)
{
    int s = wait_notbusy(CMD_MS);
    if (s < 0) {
        wsprintfA(line, "%s: %s TIMEOUT - still BSY after %d s (status %02X)", who, what, CMD_MS / 1000,
                  inb(R_CMDSTAT));
        w(line);
        return -1;
    }
    if (s & (ST_ERR | ST_DF)) {
        wsprintfA(line, "%s: %s aborted - status %02X error %02X signature LBA1/LBA2 %02X/%02X%s", who, what, s,
                  inb(R_ERROR), inb(R_LBA1), inb(R_LBA2),
                  (inb(R_LBA1) == 0x14 && inb(R_LBA2) == 0xEB) ? " (ATAPI device - a CD/DVD, not a disk)" : "");
        w(line);
        return -1;
    }
    if (!(s & ST_DRQ)) {
        wsprintfA(line, "%s: %s gave no data (status %02X)", who, what, s);
        w(line);
        return -1;
    }
    return s;
}

static void swapstr(const unsigned short *wds, int from, int n, char *out)
{
    int i, j = 0;
    for (i = from; i < from + n; i++) {
        out[j++] = (char)(wds[i] >> 8);
        out[j++] = (char)(wds[i] & 0xFF);
    }
    out[j] = 0;
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == 0)) out[--j] = 0;
}

/* [A-Za-z0-9_.-], 1-60 chars, and never a DOS device name */
static int safe_name(const char *s)
{
    static const char *const dev[] = { "CON", "PRN", "AUX", "NUL", "CLOCK$", 0 };
    char base[64];
    int n = 0, i;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
              || c == '_' || c == '.' || c == '-') || i >= 60)
            return 0;
    }
    if (i == 0) return 0;
    for (n = 0; s[n] && s[n] != '.' && n < 63; n++)
        base[n] = (s[n] >= 'a' && s[n] <= 'z') ? (char)(s[n] - 32) : s[n];
    base[n] = 0;
    for (i = 0; dev[i]; i++)
        if (!lstrcmpA(base, dev[i])) return 0;
    if (n == 4 && base[3] >= '1' && base[3] <= '9' &&
            ((base[0] == 'C' && base[1] == 'O' && base[2] == 'M') ||
             (base[0] == 'L' && base[1] == 'P' && base[2] == 'T')))
        return 0;                                /* COM1-9, LPT1-9 */
    return 1;
}

static int save_file(const char *name, const void *data, DWORD len, int replace_own)
{
    char path[128];
    HANDLE f;
    DWORD n = 0;
    BOOL ok;
    wsprintfA(path, "%s%s", OUT_DIR, name);
    if (replace_own) DeleteFileA(path);          /* only for our fixed IDENT_*.BIN names */
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { wsprintfA(line, "cannot create %s", path); w(line); return 0; }
    ok = WriteFile(f, data, len, &n, NULL);
    CloseHandle(f);
    if (!ok || n != len) { wsprintfA(line, "short write to %s", path); w(line); return 0; }
    return 1;
}

static unsigned long pci_read(unsigned bus, unsigned dev, unsigned fn, unsigned reg)
{
    outl(PCI_ADDR, 0x80000000UL | ((unsigned long)bus << 16) | ((unsigned long)dev << 11)
                   | ((unsigned long)fn << 8) | (reg & 0xFC));
    return inl(PCI_DATA);
}

/* IDENTIFY one device into ident[]. Returns 1 on success. */
static int identify(int slave, const char *who)
{
    int i;
    if (select_dev(slave, 0, who) < 0) {
        wsprintfA(line, "%s: not ready after select (absent, or not answering)", who);
        w(line);
        return 0;
    }
    outb(R_CMDSTAT, ATA_IDENTIFY);
    settle();
    if (wait_data(who, "IDENTIFY") < 0) return 0;
    for (i = 0; i < 256; i++) ident[i] = inw(R_DATA);
    return 1;
}

static void report_identity(const char *who)
{
    char model[48], serial[24], fw[12];
    unsigned long lba28 = ((unsigned long)ident[61] << 16) | ident[60];
    int lba = (ident[49] >> 9) & 1;
    int w83ok = (ident[83] & 0xC000) == 0x4000;
    swapstr(ident, 27, 20, model);
    swapstr(ident, 10, 10, serial);
    swapstr(ident, 23, 4, fw);
    wsprintfA(line, "%s: MODEL \"%s\" SERIAL \"%s\" FIRMWARE \"%s\"", who, model, serial, fw);
    w(line);
    wsprintfA(line, "%s: CHS %u/%u/%u (%lu MB by CHS); LBA supported=%d%s", who, ident[1], ident[3], ident[6],
              (unsigned long)ident[1] * ident[3] * ident[6] / 2048UL, lba,
              lba ? "" : " - LBA28 capacity below is NOT meaningful");
    w(line);
    wsprintfA(line, "%s: LBA28 sectors %lu (%lu MB)", who, lba28, lba28 / 2048UL);
    w(line);
    if (w83ok && ((ident[83] >> 10) & 1))
        wsprintfA(line, "%s: LBA48 supported, sectors (low 32 bits) %lu (%lu MB)%s", who,
                  ((unsigned long)ident[101] << 16) | ident[100],
                  (((unsigned long)ident[101] << 16) | ident[100]) / 2048UL,
                  (ident[102] || ident[103]) ? " - and more above 2^32 sectors" : "");
    else
        wsprintfA(line, "%s: LBA48 not reported (word 83 %04X)", who, ident[83]);
    w(line);
}

static void do_identify(void)
{
    int slave, s0, i, present = 0;
    unsigned long idetim;
    DeleteFileA(OUT_DIR "IDENT_M.BIN");
    DeleteFileA(OUT_DIR "IDENT_S.BIN");
    /* PIIX IDE function 00:07.1, IDETIM: primary 40h-41h, secondary 42h-43h */
    idetim = pci_read(0, 7, 1, 0x40);
    wsprintfA(line, "PCI 00:07.1 id %08lX; IDETIM primary %04lX secondary %04lX (secondary: decode enable %lu, "
              "IORDY sampling master %lu slave %lu)",
              pci_read(0, 7, 1, 0x00), idetim & 0xFFFF, (idetim >> 16) & 0xFFFF,
              (idetim >> (16 + 15)) & 1, (idetim >> (16 + 1)) & 1, (idetim >> (16 + 5)) & 1);
    w(line);
    s0 = inb(R_CMDSTAT);
    wsprintfA(line, "secondary channel status at start %02X", s0);
    w(line);
    /* sample idle: a BIOS in a DOS box driving this channel would show here */
    for (i = 0; i < 5; i++) {
        int s = inb(R_CMDSTAT);
        if (s != 0xFF && (s & (ST_BSY | ST_DRQ))) { w("channel is BUSY/DRQ - something else is using it; not touching it"); return; }
        Sleep(20);
    }
    /* presence: the task-file registers latch a value on a real channel.
     * Writing the count/LBA registers without a command is inert. */
    for (slave = 0; slave <= 1; slave++) {
        unsigned char a, b;
        outb(IDE_CTRL, CTL_NIEN);
        outb(R_DEVHEAD, (unsigned char)(0xE0 | (slave ? 0x10 : 0)));
        settle();
        outb(R_COUNT, 0x55); outb(R_LBA0, 0xAA);
        a = inb(R_COUNT); b = inb(R_LBA0);
        wsprintfA(line, "%s: register read-back 55/AA -> %02X/%02X %s", slave ? "slave" : "master", a, b,
                  (a == 0x55 && b == 0xAA) ? "(a device or latch answers)"
                  : "(no response: nothing attached, channel disabled, OR the ports are trapped)");
        w(line);
        if (a == 0x55 && b == 0xAA) present++;
    }
    for (slave = 0; slave <= 1; slave++) {
        const char *who = slave ? "slave" : "master";
        if (!identify(slave, who)) continue;
        save_file(slave ? "IDENT_S.BIN" : "IDENT_M.BIN", ident, 512, 1);
        report_identity(who);
    }
    if (!present) w("no device answered the read-back test on either position");
}

static int parse_dec(const char *s, unsigned long *out)
{
    unsigned long v = 0;
    int n = 0;
    for (; *s; s++, n++) {
        if (*s < '0' || *s > '9' || n >= 9) return 0;   /* 9 digits keeps it < 2^32 */
        v = v * 10 + (unsigned long)(*s - '0');
    }
    *out = v;
    return n > 0;
}

static void do_read(const char *swho, const char *slba, const char *scount, const char *outname)
{
    int slave;
    unsigned long lba, count, i, good = 0;
    const char *who;
    char path[128];
    HANDLE of;
    DWORD n;
    if (!lstrcmpiA(swho, "m")) slave = 0;
    else if (!lstrcmpiA(swho, "s")) slave = 1;
    else { w("read: device must be m or s"); return; }
    who = slave ? "slave" : "master";
    if (!parse_dec(slba, &lba) || !parse_dec(scount, &count) || lba > 0x0FFFFFFFUL || count == 0
            || count > MAX_READ || lba + count > 0x10000000UL || !safe_name(outname)) {
        w("read: bad arguments (lba < 2^28, count 1-128, name [A-Za-z0-9_.-], not a device name)");
        return;
    }
    if (!identify(slave, who)) { w("read: IDENTIFY failed - not reading"); return; }
    if (!((ident[49] >> 9) & 1)) { w("read: drive does not report LBA support - refusing an LBA read"); return; }
    wsprintfA(path, "%s%s", OUT_DIR, outname);
    of = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (of == INVALID_HANDLE_VALUE) { wsprintfA(line, "read: cannot create %s (exists already?)", path); w(line); return; }
    for (i = 0; i < count; i++) {
        unsigned long s = lba + i;
        int k;
        char what[40];
        if (select_dev(slave, (unsigned char)((s >> 24) & 0x0F), who) < 0) {
            wsprintfA(line, "read lba %lu: device not idle - stopping", s); w(line); break;
        }
        outb(R_COUNT, 1);
        outb(R_LBA0, (unsigned char)(s & 0xFF));
        outb(R_LBA1, (unsigned char)((s >> 8) & 0xFF));
        outb(R_LBA2, (unsigned char)((s >> 16) & 0xFF));
        outb(R_CMDSTAT, ATA_READ_SECTORS);
        settle();
        wsprintfA(what, "READ lba %lu", s);
        if (wait_data(who, what) < 0) { w("stopping at the first failure"); break; }
        for (k = 0; k < 256; k++) buf[k] = inw(R_DATA);
        if (!WriteFile(of, buf, 512, &n, NULL) || n != 512) { w("read: short write to the output file - stopping"); break; }
        good++;
    }
    CloseHandle(of);
    wsprintfA(line, "read %s lba %lu asked %lu -> %s : %lu sector(s) read", who, lba, count, path, good);
    w(line);
}

/* Leave the channel as found where safe: re-enable interrupts only when the
 * drive is idle (clearing nIEN mid-command would raise a late IRQ15). */
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
    if (!(GetVersion() & 0x80000000)) ExitProcess(5);   /* NT traps port I/O: 9x only */
    CreateDirectoryA("C:\\RETRO_AGENT", NULL);
    logf = CreateFileA(OUT_TXT, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (logf == INVALID_HANDLE_VALUE) ExitProcess(3);
    SetFilePointer(logf, 0, NULL, FILE_END);
    wsprintfA(line, "--begin-- tick %lu: %.300s", GetTickCount(), GetCommandLineA());
    w(line);
    lstrcpynA(cmdbuf, GetCommandLineA(), sizeof(cmdbuf));
    n = split(cmdbuf, tok, 8);
    if (n >= 2 && !lstrcmpiA(tok[1], "identify")) {
        do_identify();
        restore();
    } else if (n >= 6 && !lstrcmpiA(tok[1], "read")) {
        do_read(tok[2], tok[3], tok[4], tok[5]);
        restore();
    } else {
        w("usage: ide9x identify | ide9x read <m|s> <lba> <count 1-128> <name>");
    }
    w("--end--");
    CloseHandle(logf);
    ExitProcess(0);
}
