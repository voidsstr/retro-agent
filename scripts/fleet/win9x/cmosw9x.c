/*
 * cmosw9x - two Compaq Deskpro 2000 CMOS settings, set from Windows 98.
 * CRT-free, like the other 9x tools.
 *
 *   cmosw9x postskip on|off   2Dh bit 3: POST shows an error and CONTINUES
 *                             instead of waiting for F1 (on = skip the wait)
 *   cmosw9x none              1Bh 44h -> 00h: secondary IDE master type none
 *   cmosw9x restore           1Bh -> 44h
 *   cmosw9x show              read-only: log the bytes and the checksum verdict
 *
 * WHY postskip (.243, 2026-09-27): every boot halted at POST on
 * "301-Keyboard Error" and waited for F1, with a PS/2 keyboard plugged in, and
 * the box has no reachable Computer Setup. In this ROM, routine F000:95F8
 * (called once, from F000:961E at the end of POST, just before the module that
 * shows the RESUME prompt) clears the "wait for F1" flag BDA 40:12 bit 0 when
 * the CMOS checksum is valid AND CMOS 2Dh bit 3 is set. Compaq's Deskpro 4000
 * Technical Reference Guide (same BIOS generation) documents 2Dh bit 3 as
 * "POST Error Handling: 0 = Display 'Press F1 to Continue' on error,
 * 1 = Skip F1 message". Hard halts are NOT covered: 303 (keyboard controller),
 * 102, a non-bootable diskette in A:, the fixed-disk-table halt, and 162 (a bad
 * checksum still waits, by design - F000:965D).
 *
 * WHY none/restore: see scripts/fleet/win9x/README.md (the 80 GB disk). POST
 * re-types the drive at every power-on, so 'none' does not stick on .243.
 *
 * GUARDS
 *   - The data port is written only through cmos_wr(), which accepts 1Bh, 2Dh,
 *     2Eh and 2Fh, and through cmos_put_back(), which only ever writes a value
 *     read from this same register moments earlier (the repair path).
 *   - Every change is a compare-and-swap on a snapshot of 00h-7Fh: it refuses
 *     unless the stored checksum (2Eh/2Fh, the 16-bit sum of 10h..2Dh, high
 *     byte first - the ROM's own formula at F000:6049) matches the live bytes,
 *     and the new checksum is computed from those live bytes, never a constant.
 *   - Index and data writes are paced with the ROM's own delay (in al,84h).
 *   - After writing, 00h-7Fh are re-read. Every byte except the running clock
 *     (00h-09h) and the RTC's read-only C/D registers (0Ch/0Dh) must equal the
 *     intended image - 0Ah/0Bh (divider, mode) included, because a stray index
 *     between our two port writes could land a data byte there. On ANY
 *     difference, every byte is put back to the snapshot and re-verified.
 *   - The index is left at 0Dh (NMI enabled), as the ROM leaves it.
 *   - Before/after images: C:\RETRO_AGENT\CMOSB.BIN and CMOSA.BIN.
 */
#include <windows.h>

#define OUT_TXT   "C:\\RETRO_AGENT\\CMOSW9X.TXT"
#define REG_TYPE  0x1B          /* secondary master drive type (Compaq)      */
#define REG_FLAGS 0x2D          /* "Additional Flags"; bit 3 = skip F1 wait  */
#define REG_CSHI  0x2E
#define REG_CSLO  0x2F
#define FLAG_SKIP_F1 0x08
#define TYPE_WAS  0x44          /* what POST auto-typed the Seagate as       */
#define TYPE_NONE 0x00

static HANDLE logf;
static char line[512];
static unsigned char before[128], after[128], want[128];

static void w(const char *s)
{
    DWORD wr;
    WriteFile(logf, s, lstrlenA(s), &wr, NULL);
    WriteFile(logf, "\r\n", 2, &wr, NULL);
}
static unsigned char inb(unsigned short p) { unsigned char v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static void outb(unsigned short p, unsigned char v) { __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(p)); }
static void io_delay(void) { (void)inb(0x84); }     /* the ROM's own pacing */

static unsigned char cmos_rd(unsigned char idx)
{
    unsigned char v;
    outb(0x70, (unsigned char)(0x80 | idx));
    io_delay();
    v = inb(0x71);
    return v;
}
static int writable(unsigned char idx)
{
    return idx == REG_TYPE || idx == REG_FLAGS || idx == REG_CSHI || idx == REG_CSLO;
}
/* The ONLY write path for a change. Anything but the four registers is refused. */
static int cmos_wr(unsigned char idx, unsigned char v)
{
    if (!writable(idx)) return -1;
    outb(0x70, (unsigned char)(0x80 | idx));
    io_delay();
    outb(0x71, v);
    io_delay();
    return 0;
}
/* Repair only: put back the value the snapshot read from this register. The
 * clock and the RTC's read-only C/D registers are never written. */
static int cmos_put_back(unsigned char idx)
{
    if (idx < 0x0A || idx == 0x0C || idx == 0x0D || idx > 0x7F) return -1;
    outb(0x70, (unsigned char)(0x80 | idx));
    io_delay();
    outb(0x71, before[idx]);
    io_delay();
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
static int cs_valid(const unsigned char *b) { return stored_cs(b) == sum10_2d(b); }
/* bytes a compare must ignore: the running clock and the read-only C/D */
static int volatile_reg(int i) { return i <= 0x09 || i == 0x0C || i == 0x0D; }

static void save(const char *name, const void *d, DWORD n)
{
    DWORD wr; HANDLE h = CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, d, n, &wr, NULL); CloseHandle(h);
}
static void report(const char *tag, const unsigned char *b)
{
    wsprintfA(line, "%s: 0Ah=%02X 0Bh=%02X 0Eh=%02X 12h=%02X 13h=%02X 19h=%02X 1Bh=%02X 2Dh=%02X (skip-F1 %s) | "
              "checksum stored %04X computed %04X (%s)",
              tag, b[0x0A], b[0x0B], b[0x0E], b[0x12], b[0x13], b[0x19], b[REG_TYPE], b[REG_FLAGS],
              (b[REG_FLAGS] & FLAG_SKIP_F1) ? "ON" : "off", stored_cs(b), sum10_2d(b),
              cs_valid(b) ? "valid" : "INVALID");
    w(line);
}

/* Count bytes where `b` differs from `ref`, ignoring the volatile registers. */
static int diff_count(const unsigned char *b, const unsigned char *ref, int log_them)
{
    int i, n = 0;
    for (i = 0; i < 128; i++) {
        if (volatile_reg(i) || b[i] == ref[i]) continue;
        n++;
        if (log_them) {
            wsprintfA(line, "  %02Xh: expected %02X read %02X", i, ref[i], b[i]);
            w(line);
        }
    }
    return n;
}

/*
 * Write register `idx` = `val` with a checksum computed from the live bytes,
 * then prove the whole bank is exactly the intended image. On any difference,
 * put every byte back to the snapshot. Returns 0 on success.
 */
static int change(unsigned char idx, unsigned char val)
{
    unsigned cs;
    int i, bad;

    for (i = 0; i < 128; i++) want[i] = before[i];
    want[idx] = val;
    cs = sum10_2d(want);
    want[REG_CSHI] = (unsigned char)(cs >> 8);
    want[REG_CSLO] = (unsigned char)(cs & 0xFF);
    wsprintfA(line, "writing %02Xh=%02X 2Eh=%02X 2Fh=%02X", idx, val, want[REG_CSHI], want[REG_CSLO]);
    w(line);
    cmos_wr(idx, val);
    cmos_wr(REG_CSHI, want[REG_CSHI]);
    cmos_wr(REG_CSLO, want[REG_CSLO]);
    cmos_done();

    read_all(after);
    save("C:\\RETRO_AGENT\\CMOSA.BIN", after, 128);
    report("after ", after);
    bad = diff_count(after, want, 1);
    if (!cs_valid(after)) { w("MISMATCH: checksum invalid after write"); bad++; }
    if (!bad) return 0;

    /* Something is not what we wrote: restore the snapshot, byte for byte. */
    w("REPAIR: putting every changed byte back to the snapshot");
    for (i = 0; i < 128; i++)
        if (!volatile_reg(i) && after[i] != before[i]) {
            if (i == idx || i == REG_CSHI || i == REG_CSLO) cmos_wr((unsigned char)i, before[i]);
            else cmos_put_back((unsigned char)i);
        }
    cmos_done();
    read_all(after);
    save("C:\\RETRO_AGENT\\CMOSA.BIN", after, 128);
    report("repair", after);
    if (diff_count(after, before, 1) == 0 && cs_valid(after))
        w("REPAIR: CMOS is back to the snapshot - nothing changed");
    else
        w("REPAIR FAILED: CMOS differs from the snapshot - DO NOT REBOOT; investigate");
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
    {   /* COMMAND.COM may hand us trailing blanks */
        int n = lstrlenA(c);
        while (n > 0 && (c[n - 1] == ' ' || c[n - 1] == '\t' || c[n - 1] == '\r' || c[n - 1] == '\n')) c[--n] = 0;
    }

    read_all(before);
    save("C:\\RETRO_AGENT\\CMOSB.BIN", before, 128);
    report("before", before);

    if (!lstrcmpiA(c, "show")) {
        rc = 0;
    } else if (!cs_valid(before) && lstrcmpiA(c, "restore")) {
        /* Never build a change on a CMOS whose checksum does not add up. */
        w("REFUSED: the stored checksum does not match the live bytes - nothing written");
        rc = 4;
    } else if (!lstrcmpiA(c, "postskip on")) {
        if (before[REG_FLAGS] & FLAG_SKIP_F1) { w("postskip: already on - nothing written"); rc = 0; }
        else {
            rc = change(REG_FLAGS, (unsigned char)(before[REG_FLAGS] | FLAG_SKIP_F1)) ? 5 : 0;
            w(rc ? "postskip on: FAILED (see REPAIR)" : "postskip on: done - POST will show errors and continue (from the next POST)");
        }
    } else if (!lstrcmpiA(c, "postskip off")) {
        if (!(before[REG_FLAGS] & FLAG_SKIP_F1)) { w("postskip: already off - nothing written"); rc = 0; }
        else {
            rc = change(REG_FLAGS, (unsigned char)(before[REG_FLAGS] & ~FLAG_SKIP_F1)) ? 5 : 0;
            w(rc ? "postskip off: FAILED (see REPAIR)" : "postskip off: done - POST waits for F1 on errors again");
        }
    } else if (!lstrcmpiA(c, "none")) {
        if (before[REG_TYPE] != TYPE_WAS) {
            wsprintfA(line, "REFUSED: expected 1Bh=%02X - nothing written", TYPE_WAS);
            w(line);
            rc = 4;
        } else {
            rc = change(REG_TYPE, TYPE_NONE) ? 5 : 0;
            w(rc ? "none: FAILED (see REPAIR)" : "none: done - secondary master type is 00 (takes effect at the next POST)");
        }
    } else if (!lstrcmpiA(c, "restore")) {
        /* Unconditional on purpose: recovers a partial write of 1Bh. */
        rc = change(REG_TYPE, TYPE_WAS) ? 5 : 0;
        w(rc ? "restore: FAILED - do not reboot; investigate" : "restore: done - secondary master type is 44 again");
    } else {
        w("usage: cmosw9x postskip on|off | none | restore | show");
        rc = 2;
    }
    w("--end--");
    CloseHandle(logf);
    ExitProcess(rc);
}
