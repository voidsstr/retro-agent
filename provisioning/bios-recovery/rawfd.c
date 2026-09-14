/*
 * rawfd.c - write a raw floppy image to a drive, then READ IT BACK and compare.
 *
 * WHY THIS EXISTS
 * ---------------
 * Windows XP cannot write a raw floppy image: `format a: /s` does not exist on
 * the NT family and Explorer's "create an MS-DOS startup disk" is not
 * scriptable. The fleet's retro agent has UPLOAD but no raw-disk primitive, so
 * a bootable BIOS-recovery floppy could not be produced on the one machine that
 * still has a floppy drive.
 *
 * It exists in the repo rather than as a throwaway because a BIOS recovery
 * floppy is the last line of defence for a bricked board, and the next person
 * who needs one should not have to re-derive this.
 *
 * WHY IT VERIFIES BY READ-BACK
 * ----------------------------
 * A floppy is the least reliable medium in the building, and this disk is going
 * into a machine that CANNOT REPORT ANYTHING - no video, no POST codes, one
 * blind shot at a bootblock flash. "WriteFile returned success" is exactly the
 * kind of evidence this project has been burned by: the only claim worth making
 * is that the bytes were read back off the platter and compared. So the write
 * is followed by a full re-read, a byte compare, and a per-track report of any
 * mismatch. A bad sector is reported as a FAILURE, loudly, not tolerated.
 *
 * Usage:
 *   rawfd.exe <drive> <image>   e.g.  rawfd.exe A: recovery.img
 *   rawfd.exe --verify <drive> <image>    (compare only, no write)
 *
 * Build:
 *   i686-w64-mingw32-gcc -O2 -o rawfd.exe rawfd.c
 */

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512

static void die(const char *what)
{
    DWORD e = GetLastError();
    printf("FAILED: %s (GetLastError=%lu)\n", what, (unsigned long)e);
    /* Name the two failures an operator can actually act on, rather than
     * leaving them to decode a Win32 error number. */
    if (e == ERROR_WRITE_PROTECT)
        printf("  -> the diskette is WRITE PROTECTED. Slide the tab shut.\n");
    if (e == ERROR_NOT_READY || e == ERROR_FLOPPY_ID_MARK_NOT_FOUND ||
        e == ERROR_SECTOR_NOT_FOUND || e == ERROR_FLOPPY_WRONG_CYLINDER)
        printf("  -> no diskette, or it is unformatted/damaged. Try another.\n");
    exit(2);
}

/* A volume handle on a removable drive: XP will let us write it, but taking
 * the lock first stops the filesystem writing behind us mid-image. */
static HANDLE open_drive(const char *drive, int for_write)
{
    char path[16];
    _snprintf(path, sizeof path, "\\\\.\\%c:", drive[0]);
    HANDLE h = CreateFileA(path,
                           for_write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) die("CreateFile on the drive");
    if (for_write) {
        DWORD got = 0;
        /* Best effort: a floppy usually has no open handles, and a failure
         * here is not fatal - but say so rather than pretending we locked. */
        if (!DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL))
            printf("note: could not lock the volume (continuing) - close any "
                   "Explorer window on %s if this write fails\n", drive);
    }
    return h;
}

int main(int argc, char **argv)
{
    int verify_only = 0, argi = 1;
    if (argc > 1 && (!strcmp(argv[1], "--verify") || !strcmp(argv[1], "-v"))) {
        verify_only = 1; argi = 2;
    }
    if (argc - argi < 2) {
        printf("usage: rawfd.exe [--verify] <drive> <image>\n"
               "   e.g. rawfd.exe A: recovery.img\n");
        return 1;
    }
    const char *drive = argv[argi], *imgpath = argv[argi + 1];

    FILE *f = fopen(imgpath, "rb");
    if (!f) { printf("FAILED: cannot open image %s\n", imgpath); return 2; }
    fseek(f, 0, SEEK_END);
    long isz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (isz <= 0 || (isz % SECTOR)) {
        printf("FAILED: image size %ld is not a whole number of 512-byte "
               "sectors\n", isz);
        return 2;
    }
    unsigned char *img = malloc(isz), *back = malloc(isz);
    if (!img || !back) { printf("FAILED: out of memory\n"); return 2; }
    if (fread(img, 1, isz, f) != (size_t)isz) {
        printf("FAILED: short read of the image\n"); return 2;
    }
    fclose(f);
    printf("image  : %s  (%ld bytes, %ld sectors)\n", imgpath, isz, isz / SECTOR);
    printf("target : %s\n", drive);

    if (!verify_only) {
        HANDLE h = open_drive(drive, 1);
        DWORD wrote = 0, total = 0;
        printf("writing");
        fflush(stdout);
        while (total < (DWORD)isz) {
            DWORD chunk = 32 * SECTOR;                 /* 16 KB at a time */
            if (total + chunk > (DWORD)isz) chunk = (DWORD)isz - total;
            if (!WriteFile(h, img + total, chunk, &wrote, NULL) || wrote != chunk) {
                printf("\n");
                die("WriteFile to the drive");
            }
            total += wrote;
            if ((total % (128 * SECTOR)) == 0) { printf("."); fflush(stdout); }
        }
        FlushFileBuffers(h);
        CloseHandle(h);                                /* also drops the lock */
        printf(" %lu bytes written\n", (unsigned long)total);
    }

    /* THE PART THAT MATTERS: read the platter back and compare. */
    HANDLE h = open_drive(drive, 0);
    DWORD got = 0, total = 0;
    printf("verifying");
    fflush(stdout);
    while (total < (DWORD)isz) {
        DWORD chunk = 32 * SECTOR;
        if (total + chunk > (DWORD)isz) chunk = (DWORD)isz - total;
        if (!ReadFile(h, back + total, chunk, &got, NULL) || got != chunk) {
            printf("\n");
            die("ReadFile back from the drive");
        }
        total += got;
        if ((total % (128 * SECTOR)) == 0) { printf("."); fflush(stdout); }
    }
    CloseHandle(h);
    printf(" %lu bytes read\n", (unsigned long)total);

    long bad = 0, firstbad = -1;
    for (long s = 0; s < isz / SECTOR; s++) {
        if (memcmp(img + s * SECTOR, back + s * SECTOR, SECTOR)) {
            if (firstbad < 0) firstbad = s;
            bad++;
        }
    }
    if (bad) {
        printf("FAILED: %ld of %ld sectors differ (first at LBA %ld, "
               "cyl %ld head %ld sec %ld)\n",
               bad, isz / SECTOR, firstbad,
               firstbad / 36, (firstbad / 18) % 2, (firstbad % 18) + 1);
        printf("  -> this diskette is NOT trustworthy. Use a different one.\n");
        printf("RESULT: verify-failed\n");
        return 3;
    }
    printf("all %ld sectors match the image byte for byte\n", isz / SECTOR);
    printf("RESULT: ok\n");
    return 0;
}
