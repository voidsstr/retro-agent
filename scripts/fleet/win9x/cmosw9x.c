/*
 * cmosw9x - set or restore the Compaq Deskpro 2000's secondary-IDE-master
 * drive type in CMOS, from Windows 98. CRT-free, like the other 9x tools.
 *
 *   cmosw9x none      1Bh 44h -> 00h (compare-and-swap; refuses any other state)
 *   cmosw9x restore   1Bh -> 44h, unconditionally (recovers a partial write)
 *   cmosw9x show      read-only: log the bytes and the checksum verdict
 *
 * WHY (found on .243, 2026-09-26): the BIOS auto-typed an 80 GB disk on the
 * secondary master as type 68, whose logical-heads byte is 00 (256 heads,
 * overflowed). Its INT 13h CHS validator then rejects every read with a head
 * above 0, ESDI_506's BIOS verify read fails ("ESDI BIOS read failure") and
 * the whole secondary channel is torn down (Config Manager Problem 10). With
 * 1Bh = 00 the BIOS creates no unit for the disk and ESDI_506 claims it from
 * IDENTIFY, LBA28, with no BIOS read at all. The analysis is in
 * scripts/fleet/win9x/README.md.
 *
 * GUARDS
 *   - Writes exactly three registers: 1Bh (the drive type) and 2Eh/2Fh (the
 *     standard checksum, the 16-bit sum of 10h..2Dh, high byte first, the
 *     formula the ROM itself uses at F000:6049). Nothing else is ever written;
 *     Compaq's second checksum over 50h-9Dh (9Eh/9Fh) is outside that range.
 *   - `none` refuses unless 1Bh = 44h AND the stored checksum equals the
 *     computed one, so it can never act on a CMOS it does not understand.
 *   - After writing it re-reads 00h-7Fh and fails loudly unless exactly those
 *     bytes changed (the clock registers 00h-0Ch tick, so they are excluded)
 *     and the checksum is valid.
 *   - Each index/data pair is issued back to back with the NMI-disable bit set;
 *     the index is left at 0Dh with NMI enabled, as BIOS9X and the ROM do.
 *   - Before/after images: C:\RETRO_AGENT\CMOSB.BIN and CMOSA.BIN.
 */
#include <windows.h>

#define OUT_TXT   "C:\\RETRO_AGENT\\CMOSW9X.TXT"
#define REG_TYPE  0x1B          /* secondary master drive type (Compaq)      */
#define REG_CSHI  0x2E
#define REG_CSLO  0x2F
#define TYPE_WAS  0x44          /* what POST auto-typed the Seagate as       */
#define TYPE_NONE 0x00

static HANDLE logf;
static char line[512];
static unsigned char before[128], after[128];

static void w(const char *s)
{
    DWORD wr;
    WriteFile(logf, s, lstrlenA(s), &wr, NULL);
    WriteFile(logf, "\r\n", 2, &wr, NULL);
}
static unsigned char inb(unsigned short p) { unsigned char v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static void outb(unsigned short p, unsigned char v) { __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(p)); }

static unsigned char cmos_rd(unsigned char idx)
{
    unsigned char v;
    outb(0x70, (unsigned char)(0x80 | idx));
    v = inb(0x71);
    return v;
}
/* The ONLY write path. Anything but the three registers is refused. */
static int cmos_wr(unsigned char idx, unsigned char v)
{
    if (idx != REG_TYPE && idx != REG_CSHI && idx != REG_CSLO) return -1;
    outb(0x70, (unsigned char)(0x80 | idx));
    outb(0x71, v);
    return 0;
}
static void cmos_done(void) { outb(0x70, 0x0D); }

static void read_all(unsigned char *b)
{
    int i;
    for (i = 0; i < 128; i++) b[i] = cmos_rd((unsigned char)i);
    cmos_done();
}
static unsigned sum10_2d(const unsigned char *b)
{
    unsigned s = 0; int i;
    for (i = 0x10; i <= 0x2D; i++) s += b[i];
    return s & 0xFFFF;
}
static unsigned stored_cs(const unsigned char *b) { return ((unsigned)b[REG_CSHI] << 8) | b[REG_CSLO]; }

static void save(const char *name, const void *d, DWORD n)
{
    DWORD wr; HANDLE h = CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, d, n, &wr, NULL); CloseHandle(h);
}
static void report(const char *tag, const unsigned char *b)
{
    wsprintfA(line, "%s: 12h=%02X 19h=%02X 1Ah=%02X 1Bh=%02X 1Ch=%02X 0Dh=%02X 0Eh=%02X | checksum stored %04X computed %04X (%s)",
              tag, b[0x12], b[0x19], b[0x1A], b[REG_TYPE], b[0x1C], b[0x0D], b[0x0E], stored_cs(b), sum10_2d(b),
              stored_cs(b) == sum10_2d(b) ? "valid" : "INVALID");
    w(line);
}

/* Set 1Bh to <type> and the checksum to match; then prove only those changed. */
static int set_type(unsigned char type)
{
    unsigned char want[128];
    unsigned cs;
    int i, bad = 0;
    for (i = 0; i < 128; i++) want[i] = before[i];
    want[REG_TYPE] = type;
    cs = sum10_2d(want);
    want[REG_CSHI] = (unsigned char)(cs >> 8);
    want[REG_CSLO] = (unsigned char)(cs & 0xFF);
    wsprintfA(line, "writing 1Bh=%02X 2Eh=%02X 2Fh=%02X", want[REG_TYPE], want[REG_CSHI], want[REG_CSLO]);
    w(line);
    cmos_wr(REG_TYPE, want[REG_TYPE]);
    cmos_wr(REG_CSHI, want[REG_CSHI]);
    cmos_wr(REG_CSLO, want[REG_CSLO]);
    cmos_done();
    read_all(after);
    save("C:\\RETRO_AGENT\\CMOSA.BIN", after, 128);
    report("after ", after);
    for (i = 0x0D; i < 128; i++)            /* 00h-0Ch are the running clock */
        if (after[i] != want[i]) {
            wsprintfA(line, "MISMATCH at %02Xh: want %02X read %02X", i, want[i], after[i]);
            w(line);
            bad++;
        }
    if (stored_cs(after) != sum10_2d(after)) { w("MISMATCH: checksum invalid after write"); bad++; }
    return bad;
}

void __stdcall start(void)
{
    char *c = GetCommandLineA();
    int rc = 0;
    logf = CreateFileA(OUT_TXT, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (logf == INVALID_HANDLE_VALUE) ExitProcess(3);
    SetFilePointer(logf, 0, NULL, FILE_END);
    wsprintfA(line, "--begin-- tick %lu: %.200s", GetTickCount(), c);
    w(line);
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; } else while (*c && *c != ' ') c++;
    while (*c == ' ') c++;

    read_all(before);
    save("C:\\RETRO_AGENT\\CMOSB.BIN", before, 128);
    report("before", before);

    if (!lstrcmpiA(c, "show")) {
        rc = 0;
    } else if (!lstrcmpiA(c, "none")) {
        if (before[REG_TYPE] != TYPE_WAS || stored_cs(before) != sum10_2d(before)) {
            wsprintfA(line, "REFUSED: expected 1Bh=%02X with a valid checksum - nothing written", TYPE_WAS);
            w(line);
            rc = 4;
        } else {
            rc = set_type(TYPE_NONE) ? 5 : 0;
            w(rc ? "none: FAILED - run 'cmosw9x restore' before any reboot" : "none: done - secondary master type is 00 (takes effect at the next POST)");
        }
    } else if (!lstrcmpiA(c, "restore")) {
        rc = set_type(TYPE_WAS) ? 5 : 0;
        w(rc ? "restore: FAILED - do not reboot; investigate" : "restore: done - secondary master type is 44 again");
    } else {
        w("usage: cmosw9x none | restore | show");
        rc = 2;
    }
    w("--end--");
    CloseHandle(logf);
    ExitProcess(rc);
}
