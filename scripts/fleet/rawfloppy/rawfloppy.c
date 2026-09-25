/*
 * rawfloppy.exe - write / verify a raw floppy image on a Windows fleet box.
 *
 * Windows NT has no way to lay a boot sector on a floppy from the command
 * line: XP's format.com dropped /s, and "Create an MS-DOS startup disk" is
 * Explorer-only.  So a bootable floppy is made by building the whole 1.44 MB
 * image on the Linux host and writing it here, sector for sector.
 *
 *   rawfloppy write  A: image.img     - write the image to the disk
 *   rawfloppy verify A: image.img     - read the disk back and compare
 *   rawfloppy read   A: out.img       - dump the disk to a file
 *
 * Exit code 0 on success, non-zero on failure.  Every failure names the
 * Windows error and the byte offset it happened at - a write that stops
 * half way must not look like a write that finished.
 */
#include <windows.h>
#include <stdio.h>

#define CHUNK (512 * 18)   /* one track */

static HANDLE open_volume(const char *drv, int for_write)
{
    char path[16];
    HANDLE h;
    DWORD ret;

    wsprintfA(path, "\\\\.\\%c:", drv[0]);
    h = CreateFileA(path,
                    for_write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("ERROR: cannot open %s (error %lu)\n", path, GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    if (for_write) {
        /* Lock so the filesystem cannot write behind us, then dismount so
         * the cached FAT of the OLD disk is discarded. */
        if (!DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &ret, NULL))
            printf("WARN: lock volume failed (error %lu) - continuing\n",
                   GetLastError());
        if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &ret, NULL))
            printf("WARN: dismount failed (error %lu) - continuing\n",
                   GetLastError());
    }
    return h;
}

int main(int argc, char **argv)
{
    const char *op, *drv, *file;
    HANDLE hv, hf;
    unsigned char buf[CHUNK], cmp[CHUNK];
    DWORD got, did;
    DWORD total = 0;
    int bad = 0;

    if (argc != 4) {
        printf("usage: rawfloppy <write|verify|read> <drive:> <image>\n");
        return 2;
    }
    op = argv[1]; drv = argv[2]; file = argv[3];

    if (lstrcmpiA(op, "read") == 0) {
        hv = open_volume(drv, 0);
        if (hv == INVALID_HANDLE_VALUE) return 1;
        hf = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (hf == INVALID_HANDLE_VALUE) {
            printf("ERROR: cannot create %s (error %lu)\n", file, GetLastError());
            CloseHandle(hv); return 1;
        }
        while (ReadFile(hv, buf, CHUNK, &got, NULL) && got > 0) {
            if (!WriteFile(hf, buf, got, &did, NULL) || did != got) {
                printf("ERROR: write to %s failed at %lu (error %lu)\n",
                       file, total, GetLastError());
                bad = 1; break;
            }
            total += got;
        }
        CloseHandle(hf); CloseHandle(hv);
        printf("%s: %lu bytes read from %s\n", bad ? "FAILED" : "OK", total, drv);
        return bad;
    }

    hf = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        printf("ERROR: cannot open %s (error %lu)\n", file, GetLastError());
        return 1;
    }
    hv = open_volume(drv, lstrcmpiA(op, "write") == 0);
    if (hv == INVALID_HANDLE_VALUE) { CloseHandle(hf); return 1; }

    while (ReadFile(hf, buf, CHUNK, &got, NULL) && got > 0) {
        if (lstrcmpiA(op, "write") == 0) {
            if (!WriteFile(hv, buf, got, &did, NULL) || did != got) {
                printf("ERROR: write failed at byte %lu (error %lu)\n",
                       total, GetLastError());
                bad = 1; break;
            }
        } else {
            if (!ReadFile(hv, cmp, got, &did, NULL) || did != got) {
                printf("ERROR: read failed at byte %lu (error %lu)\n",
                       total, GetLastError());
                bad = 1; break;
            }
            if (memcmp(buf, cmp, got) != 0) {
                printf("ERROR: MISMATCH in the track at byte %lu\n", total);
                bad = 1; break;
            }
        }
        total += got;
    }
    FlushFileBuffers(hv);
    CloseHandle(hv);
    CloseHandle(hf);

    if (!bad && total == 0) { printf("FAILED: image was empty\n"); return 1; }
    printf("%s: %lu bytes %s %s\n", bad ? "FAILED" : "OK", total,
           lstrcmpiA(op, "write") == 0 ? "written to" : "verified against", drv);
    return bad;
}
