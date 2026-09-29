/* cdimage - make an exact .iso of a data CD from the raw volume (Windows XP+).
 *
 *   cdimage <drive letter> <output .iso path> [log path] [first count]
 *     first/count: image ONLY that sector range (a diagnostic - e.g. the
 *     directory area, to find which file a bad sector belongs to); the
 *     result is then not an image of the disc and says so
 *
 * Reads \\.\X: in 2048-byte sectors: everything the disc's ISO9660 filesystem
 * spans (the Primary Volume Descriptor's volume space size, sector 16), which
 * includes the El Torito boot record and boot image, and then keeps reading
 * past it until the drive refuses, so nothing the volume holds is dropped.
 * An MD5 of exactly the bytes written is computed as they are read (CryptoAPI)
 * so the copy on the share can be checked against the disc from another host.
 * A sector the drive cannot read is retried 3 times and then FAILS the run -
 * a silently padded image is worse than none.
 *
 * Written 2026-09-29 to image the Windows 98 SE CD in .110 onto the NAS, for
 * the 86Box Win98 build VM. Build: i686-w64-mingw32-gcc -O2 -o cdimage.exe
 * cdimage.c -ladvapi32 */
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>

#define SEC 2048
#define CHUNK 32                     /* sectors per read: 64 KB */

static FILE *lg;
static void say(const char *fmt, ...)
{
    char b[512]; va_list a;
    va_start(a, fmt); _vsnprintf(b, sizeof b - 1, fmt, a); va_end(a); b[sizeof b - 1] = 0;
    printf("%s\n", b); fflush(stdout);
    if (lg) { fprintf(lg, "%s\r\n", b); fflush(lg); }
}

static int rd(HANDLE h, unsigned long lba, unsigned n, unsigned char *buf)
{
    LARGE_INTEGER off; DWORD got = 0; int t;
    for (t = 0; t < 3; t++) {
        off.QuadPart = (LONGLONG)lba * SEC;
        if (SetFilePointerEx(h, off, NULL, FILE_BEGIN) && ReadFile(h, buf, n * SEC, &got, NULL) && got == n * SEC)
            return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    char dev[16]; HANDLE h, out; unsigned char *buf; unsigned long vol, lba = 0, extra = 0;
    HCRYPTPROV cp; HCRYPTHASH ch; BYTE md5[16]; DWORD ml = 16, w; int i;
    if (argc < 3) { printf("usage: cdimage <drive letter> <out.iso> [log]\n"); return 3; }
    unsigned long first = 0, count = 0;
    if (argc > 3) lg = fopen(argv[3], "a");
    if (argc > 5) { first = strtoul(argv[4], NULL, 0); count = strtoul(argv[5], NULL, 0); }
    sprintf(dev, "\\\\.\\%c:", argv[1][0]);
    h = CreateFileA(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (h == INVALID_HANDLE_VALUE) { say("FAIL: open %s: error %lu", dev, GetLastError()); return 2; }
    buf = VirtualAlloc(NULL, CHUNK * SEC, MEM_COMMIT, PAGE_READWRITE);
    if (rd(h, 16, 1, buf) || memcmp(buf + 1, "CD001", 5) || buf[0] != 1) { say("FAIL: sector 16 is not an ISO9660 Primary Volume Descriptor"); return 2; }
    vol = buf[80] | (buf[81] << 8) | (buf[82] << 16) | ((unsigned long)buf[83] << 24);
    say("PVD: volume id \"%.32s\", volume space %lu sectors (%lu bytes)", buf + 40, vol, vol * SEC);
    out = CreateFileA(argv[2], GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) { say("FAIL: create %s: error %lu (it must not exist)", argv[2], GetLastError()); return 2; }
    if (!CryptAcquireContextA(&cp, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) || !CryptCreateHash(cp, CALG_MD5, 0, 0, &ch)) { say("FAIL: CryptoAPI"); return 2; }
    if (count) { lba = first; vol = first + count; say("RANGE ONLY: sectors %lu..%lu - NOT a whole-disc image", first, vol - 1); }
    while (lba < vol) {
        unsigned n = (vol - lba) < CHUNK ? (unsigned)(vol - lba) : CHUNK;
        if (rd(h, lba, n, buf)) {                 /* narrow down to the bad sector */
            unsigned k;
            /* A marginal disc fails a sector at streaming speed that reads fine
             * alone (.110's Win98 SE CD, sector 62656: 3/3 chunk reads failed,
             * 4/4 single reads identical). CD data sectors carry EDC/ECC, so a
             * read that succeeds is the sector - patience cannot make it wrong. */
            for (k = 0; k < n; k++) {
                int t2, ok = 0;
                for (t2 = 0; t2 < 10 && !ok; t2++) { if (t2) Sleep(500); ok = !rd(h, lba + k, 1, buf + k * SEC); }
                if (!ok) { say("FAIL: sector %lu unreadable (30 tries) - no image", lba + k); CloseHandle(out); DeleteFileA(argv[2]); return 2; }
                if (t2 > 1) say("  sector %lu read on single-sector attempt %d", lba + k, t2);
            }
        }
        if (!WriteFile(out, buf, n * SEC, &w, NULL) || w != n * SEC) { say("FAIL: write at sector %lu: error %lu", lba, GetLastError()); CloseHandle(out); DeleteFileA(argv[2]); return 2; }
        CryptHashData(ch, buf, n * SEC, 0);
        lba += n;
        if (lba % 16384 == 0) say("  %lu / %lu sectors", lba, vol);
    }
    /* anything readable past the filesystem's end belongs to the volume too */
    while (!count && !rd(h, lba, 1, buf)) {
        WriteFile(out, buf, SEC, &w, NULL); CryptHashData(ch, buf, SEC, 0); lba++; extra++;
        if (extra > 65536) break;
    }
    CryptGetHashParam(ch, HP_HASHVAL, md5, &ml, 0);
    CloseHandle(out); CloseHandle(h);
    {
        char hex[33];
        for (i = 0; i < 16; i++) sprintf(hex + 2 * i, "%02x", md5[i]);
        say("OK: %lu sectors (%lu past the filesystem), %lu bytes, md5 %s -> %s", lba - first, extra, (lba - first) * SEC, hex, argv[2]);
    }
    return 0;
}
