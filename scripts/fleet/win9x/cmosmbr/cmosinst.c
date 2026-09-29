/* CMOSINST - install / remove / check the cmosmbr stage on BIOS disk 80h.
 * REAL-MODE DOS ONLY (Windows 98's MS-DOS mode or a boot floppy/CD): it uses
 * INT 13h directly, which a Windows DOS box does not pass to the disk.
 *
 *   CMOSINST CHECK     report only
 *   CMOSINST INSTALL   LBA SAVE_LBA := the current MBR (verified), then LBA0 := stage
 *                      code + this disk's own stamp (DAh..DFh), signature and
 *                      partition table (verified)
 *   CMOSINST REMOVE    LBA0 := the saved code from LBA SAVE_LBA + the live stamp
 *                      and table (verified), then LBA SAVE_LBA := zeros
 * Every step is logged to C:\CMOSMBR\CMOSMBR.TXT; the first INSTALL also
 * saves the untouched MBR as C:\CMOSMBR\MBRORIG.BIN.
 * Refuses: no 55AA, a partition starting at or below LBA SAVE_LBA, a non-empty
 * LBA SAVE_LBA, any read/verify mismatch. LBA 1..SAVE_LBA-1 are never written. Recovery if the disk will not
 * boot: boot the Windows 98 CD and run FDISK /MBR (keeps the partition table).
 * Exit: 0 ok/installed, 1 refused, 2 disk error, 3 usage. */
#include <stdio.h>
#include <string.h>
#include <i86.h>
#include "mbrcode.h"
#ifndef SAVE_SEC
#error SAVE_SEC (the CHS sector of the saved MBR) comes from the Makefile
#endif
#define SAVE_LBA (SAVE_SEC - 1)

static unsigned char buf_raw[3 * 512 + 512];
static unsigned char *s0, *s1, *io;          /* LBA0 copy, LBA1 copy, I/O buffer */
static FILE *lg;
static unsigned last_ah;          /* INT 13h status of the last failure */

static void say(const char *m)
{
    printf("%s\n", m);
    if (lg) { fprintf(lg, "%s\r\n", m); fflush(lg); }
}

/* one sector at CHS 0/0/s of drive 80h; ah=2 read, 3 write */
static int bios(int ah, int sec, unsigned char *b)
{
    union REGS r; struct SREGS sr; int tries;
    for (tries = 0; tries < 3; tries++) {
        segread(&sr);
        r.h.ah = ah; r.h.al = 1; r.h.ch = 0; r.h.cl = sec; r.h.dh = 0; r.h.dl = 0x80;
        sr.es = FP_SEG(b); r.x.bx = FP_OFF(b);
        int86x(0x13, &r, &r, &sr);
        if (!r.x.cflag) return 0;
        last_ah = r.h.ah;
        r.h.ah = 0; r.h.dl = 0x80; int86(0x13, &r, &r);
    }
    return -1;
}

static int is_zero(const unsigned char *b) { int i; for (i = 0; i < 512; i++) if (b[i]) return 0; return 1; }
static int has_sig(const unsigned char *b) { return b[510] == 0x55 && b[511] == 0xAA; }
static int is_ours(const unsigned char *b)
{
    return !memcmp(b, mbr_code, 0xDA) && !memcmp(b + 0xE0, mbr_code + 0xE0, 0x1B8 - 0xE0);
}

static int write_verified(int sec, const unsigned char *want, const char *what)
{
    char m[96];
    memcpy(io, want, 512);
    if (bios(3, sec, io)) { sprintf(m, "FAIL: write %s (LBA %d) - INT 13h status %02Xh", what, sec - 1, last_ah); say(m); return -1; }
    memset(io, 0xEE, 512);
    if (bios(2, sec, io) || memcmp(io, want, 512)) {
        sprintf(m, "FAIL: %s (LBA %d) did not read back as written", what, sec - 1); say(m); return -1;
    }
    sprintf(m, "ok: %s written to LBA %d and read back", what, sec - 1); say(m);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned char n0[512];
    char m[128];
    int i, mode;
    unsigned long start;
    if (argc != 2) { printf("usage: CMOSINST CHECK|INSTALL|REMOVE\n"); return 3; }
    if (!stricmp(argv[1], "CHECK")) mode = 0;
    else if (!stricmp(argv[1], "INSTALL")) mode = 1;
    else if (!stricmp(argv[1], "REMOVE")) mode = 2;
    else { printf("usage: CMOSINST CHECK|INSTALL|REMOVE\n"); return 3; }
    lg = fopen("C:\\CMOSMBR\\CMOSMBR.TXT", "a");
    /* sector buffers that do not cross a 64 KB physical boundary */
    {
        unsigned char *p = buf_raw;
        unsigned long phys = ((unsigned long)FP_SEG(p) << 4) + FP_OFF(p);
        if ((phys & 0xFFFFUL) > 0x10000UL - 3 * 512) p += 512;
        s0 = p; s1 = p + 512; io = p + 1024;
    }
    sprintf(m, "cmosinst %s", argv[1]); say(m);
    if (bios(2, 1, s0)) { sprintf(m, "FAIL: cannot read LBA 0 (INT 13h status %02Xh)", last_ah); say(m); return 2; }
    if (bios(2, SAVE_SEC, s1)) { sprintf(m, "FAIL: cannot read LBA %d (INT 13h status %02Xh)", SAVE_LBA, last_ah); say(m); return 2; }
    if (!has_sig(s0)) { say("REFUSED: LBA 0 has no 55AA signature"); return 1; }
    for (i = 0; i < 4; i++) {
        const unsigned char *e = s0 + 0x1BE + 16 * i;
        start = e[8] | ((unsigned long)e[9] << 8) | ((unsigned long)e[10] << 16) | ((unsigned long)e[11] << 24);
        sprintf(m, "partition %d: boot %02X type %02X start %lu", i + 1, e[0], e[4], start); say(m);
        if (e[4] && start <= SAVE_LBA) { say("REFUSED: a partition starts at or below the save sector"); return 1; }
    }
    sprintf(m, "LBA 0: stage %s; stamp %02X %02X %02X %02X %02X %02X", is_ours(s0) ? "INSTALLED" : "absent",
            s0[0xDA], s0[0xDB], s0[0xDC], s0[0xDD], s0[0xDE], s0[0xDF]); say(m);
    sprintf(m, "LBA %d (save sector): %s", SAVE_LBA, is_zero(s1) ? "empty" : has_sig(s1) ? "a saved MBR (55AA)" : "NOT EMPTY"); say(m);
    if (mode == 0) {
        /* CHECK also leaves LBA 0, LBA 1 and the save sector (SAVE.BIN) as files, and maps the rest of
         * track 0 (LBA 2..62: CHS 0/0/3..63), so what is on the disk can be
         * read from Windows - VWIN32 will not serve hard disks on 9x. */
        FILE *f;
        char map[64];
        if ((f = fopen("C:\\CMOSMBR\\LBA0.BIN", "wb")) != NULL) { fwrite(s0, 1, 512, f); fclose(f); }
        if ((f = fopen("C:\\CMOSMBR\\SAVE.BIN", "wb")) != NULL) { fwrite(s1, 1, 512, f); fclose(f); }
        if (!bios(2, 2, io) && (f = fopen("C:\\CMOSMBR\\LBA1.BIN", "wb")) != NULL) { fwrite(io, 1, 512, f); fclose(f); }
        for (i = 3; i <= 63; i++) {
            if (bios(2, i, io)) map[i - 3] = 'e';
            else map[i - 3] = is_zero(io) ? '.' : has_sig(io) ? 'S' : 'D';
        }
        map[61] = 0;
        sprintf(m, "LBA 2..62 (. zero, D data, S 55AA, e error): %s", map); say(m);
        return 0;
    }
    if (mode == 1) {
        if (is_ours(s0)) { say("already installed - nothing written"); return 0; }
        if (!is_zero(s1)) { say("REFUSED: the save sector is not empty - something else may live there"); return 1; }
        if (lg) { FILE *f = fopen("C:\\CMOSMBR\\MBRORIG.BIN", "rb");
                  if (!f) { f = fopen("C:\\CMOSMBR\\MBRORIG.BIN", "wb"); if (f) { fwrite(s0, 1, 512, f); fclose(f); say("saved C:\\CMOSMBR\\MBRORIG.BIN"); } }
                  else fclose(f); }
        if (write_verified(SAVE_SEC, s0, "original MBR")) return 2;
        memcpy(n0, mbr_code, 440);
        memcpy(n0 + 0xDA, s0 + 0xDA, 6);          /* this disk's Win9x stamp */
        memcpy(n0 + 0x1B8, s0 + 0x1B8, 512 - 0x1B8); /* signature, table, 55AA */
        if (write_verified(1, n0, "cmosmbr stage")) {
            say("stage write failed - restoring the original MBR to LBA 0");
            write_verified(1, s0, "original MBR");
            return 2;
        }
        say("INSTALLED");
        return 0;
    }
    /* REMOVE */
    if (!is_ours(s0)) { say("not installed - nothing written"); return 0; }
    if (!has_sig(s1)) { say("REFUSED: the save sector holds no saved MBR - use FDISK /MBR"); return 1; }
    memcpy(n0, s1, 0x1B8);
    memcpy(n0 + 0xDA, s0 + 0xDA, 6);
    memcpy(n0 + 0x1B8, s0 + 0x1B8, 512 - 0x1B8);
    if (write_verified(1, n0, "original MBR code")) return 2;
    memset(n0, 0, 512);
    if (write_verified(SAVE_SEC, n0, "empty save sector")) return 2;
    say("REMOVED");
    return 0;
}
