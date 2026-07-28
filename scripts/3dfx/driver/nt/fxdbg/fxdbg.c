/*
 * fxdbg.exe - the M4d on-card BRING-UP LADDER driver (user-mode tool).
 *
 * This is the "tiny user tool at M4d" the gbkernel design (docs/
 * 3dfx-gbkernel-design.md sec 6, and scripts/3dfx/driver/nt/gbkdebug.h) calls
 * for: it drives the kernel-mode Glide backend's private DrvEscape opcodes over
 * ExtEscape on the display DC, ONE RUNG AT A TIME, and prints the result buffers
 * as plain ASCII so the retro agent (or a human at the box) can validate the
 * transport on real Voodoo3 hardware WITHOUT Direct3D.
 *
 * It touches no card state itself - every card touch happens inside the display
 * driver (fxd3ddd.dll / gbkernel.c). This tool only issues escapes and decodes
 * the fixed structs in gbkdebug.h, so it is safe to build and ship ahead of the
 * hardware step; the ladder it drives IS the M4d hardware regression.
 *
 * Build (mingw, host): make -C scripts/3dfx/driver/nt/fxdbg
 * Run (on the box, once fxd3ddd.dll is the active display driver):
 *     fxdbg probe
 *     fxdbg clear 0 128 255
 *     fxdbg tri
 *     fxdbg tex
 *     fxdbg readback back 0 0 64 64 out.bmp
 *     fxdbg ladder            (runs rungs 1..5 in order, PASS/FAIL each)
 *
 * The exit code is 0 only when every requested rung reported ok and unfaulted,
 * so it can gate an automated deploy/verify step.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gbkdebug.h"

/* ExtEscape opcodes are ints; our 0x3DF0.. range fits. */
#define ESC(x) ((int)(x))

/* Open the primary display DC (that's the DC DrvEscape is dispatched on). */
static HDC
open_display_dc(void)
{
    /* CreateDC("DISPLAY",...) targets the primary display device. */
    HDC hdc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    if (hdc == NULL)
        fprintf(stderr, "fxdbg: CreateDC(\"DISPLAY\") failed (err=%lu)\n",
                (unsigned long)GetLastError());
    return hdc;
}

/* QUERYESCSUPPORT (standard GDI escape 8): does the active driver claim opc? */
static int
query_supported(HDC hdc, unsigned long opc)
{
    int q = (int)opc;
    /* ExtEscape(hdc, QUERYESCSUPPORT, sizeof(escnum), &escnum, 0, NULL) */
    int r = ExtEscape(hdc, 8, (int)sizeof(q), (LPCSTR)&q, 0, NULL);
    return r > 0;
}

static void
print_probe(const fxdbg_probe_t *p)
{
    printf("  magic         0x%08lX %s\n", p->magic,
           p->magic == FXDBG_MAGIC ? "(FXDB ok)" : "(BAD - not our driver!)");
    printf("  attached      %lu\n", p->attached);
    printf("  faulted       %lu\n", p->faulted);
    printf("  status3d      0x%08lX\n", p->status3d);
    printf("  statusio      0x%08lX\n", p->statusio);
    printf("  fifoReadPtr   0x%08lX\n", p->fifoReadPtr);
    printf("  fifoBaseSize  0x%08lX\n", p->fifoBaseSize);
    printf("  fifoDepth     %lu\n", p->fifoDepth);
    printf("  vramBytes     %lu (%lu MB)\n", p->vramBytes, p->vramBytes >> 20);
    printf("  desktopEnd    0x%08lX\n", p->desktopEnd);
    printf("  desktopStride %lu\n", p->desktopStride);
    printf("  tramOffset    0x%08lX  tramSize   %lu\n", p->tramOffset, p->tramSize);
    printf("  fifoOffset    0x%08lX  fifoLength %lu\n", p->fifoOffset, p->fifoLength);
    printf("  stride(3d)    %lu\n", p->stride);
    printf("  colBuf0/1/2   0x%08lX 0x%08lX 0x%08lX (n=%lu)\n",
           p->colBuf0, p->colBuf1, p->colBuf2, p->nColBuffers);
    printf("  auxOffset     0x%08lX (n=%lu)\n", p->auxOffset, p->nAuxBuffers);
    printf("  backIdx       %lu\n", p->backIdx);
    printf("  width/height  %lu x %lu\n", p->width, p->height);
    printf("  depthBytes    %lu\n", p->depthBytes);
    printf("  swWriteOfs    0x%08lX  swReadOfs 0x%08lX\n",
           p->swWriteOfs, p->swReadOfs);
}

static int
do_probe(HDC hdc, int verbose)
{
    fxdbg_probe_t p;
    int r;
    memset(&p, 0, sizeof(p));
    r = ExtEscape(hdc, ESC(FXDBG_PROBE), 0, NULL,
                  (int)sizeof(p), (LPSTR)&p);
    if (r != (int)sizeof(p)) {
        fprintf(stderr, "fxdbg: PROBE escape returned %d (expected %d) - "
                "driver not active or escape unsupported\n",
                r, (int)sizeof(p));
        return 0;
    }
    if (verbose) {
        printf("[rung 1] PROBE:\n");
        print_probe(&p);
    }
    if (p.magic != FXDBG_MAGIC) {
        fprintf(stderr, "fxdbg: PROBE magic mismatch - not the fxD3D driver\n");
        return 0;
    }
    if (!p.attached) {
        fprintf(stderr, "fxdbg: backend reports NOT attached (2D-only surface)\n");
        return 0;
    }
    if (p.faulted) {
        fprintf(stderr, "fxdbg: backend FAULTED (FIFO wedge latched)\n");
        return 0;
    }
    return 1;
}

/* Rungs 2-4 share the small status result. Returns 1 on ok+unfaulted. */
static int
do_status_rung(HDC hdc, unsigned long opc, const void *pvIn, int cjIn,
               const char *label)
{
    fxdbg_status_out_t out;
    int r;
    memset(&out, 0, sizeof(out));
    r = ExtEscape(hdc, ESC(opc), cjIn, (LPCSTR)pvIn,
                  (int)sizeof(out), (LPSTR)&out);
    if (r != (int)sizeof(out)) {
        fprintf(stderr, "fxdbg: %s escape returned %d (expected %d)\n",
                label, r, (int)sizeof(out));
        return 0;
    }
    printf("  %-8s magic=0x%08lX ok=%lu faulted=%lu\n",
           label, out.magic, out.ok, out.faulted);
    if (out.magic != FXDBG_MAGIC) {
        fprintf(stderr, "fxdbg: %s magic mismatch\n", label);
        return 0;
    }
    return out.ok && !out.faulted;
}

/* Write a 16bpp-565 readback buffer out as a 24bpp bottom-up BMP. */
static int
write_bmp565(const char *path, const unsigned short *px,
             unsigned long w, unsigned long h)
{
    FILE *f;
    unsigned long rowbytes = ((w * 3u) + 3u) & ~3u;   /* DWORD-aligned rows */
    unsigned long imgsize  = rowbytes * h;
    unsigned long fsize    = 54u + imgsize;
    unsigned char hdr[54];
    unsigned char *row;
    unsigned long x, y;

    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "fxdbg: cannot open %s\n", path); return 0; }

    memset(hdr, 0, sizeof(hdr));
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=(unsigned char)(fsize); hdr[3]=(unsigned char)(fsize>>8);
    hdr[4]=(unsigned char)(fsize>>16); hdr[5]=(unsigned char)(fsize>>24);
    hdr[10]=54;                       /* pixel data offset                 */
    hdr[14]=40;                       /* DIB header size                   */
    hdr[18]=(unsigned char)(w); hdr[19]=(unsigned char)(w>>8);
    hdr[20]=(unsigned char)(w>>16); hdr[21]=(unsigned char)(w>>24);
    hdr[22]=(unsigned char)(h); hdr[23]=(unsigned char)(h>>8);
    hdr[24]=(unsigned char)(h>>16); hdr[25]=(unsigned char)(h>>24);
    hdr[26]=1;                        /* planes                            */
    hdr[28]=24;                       /* bpp                               */
    hdr[34]=(unsigned char)(imgsize); hdr[35]=(unsigned char)(imgsize>>8);
    hdr[36]=(unsigned char)(imgsize>>16); hdr[37]=(unsigned char)(imgsize>>24);
    fwrite(hdr, 1, 54, f);

    row = (unsigned char *)malloc(rowbytes);
    if (!row) { fclose(f); return 0; }
    /* BMP is bottom-up: emit last source row first. */
    for (y = 0; y < h; ++y) {
        const unsigned short *src = px + (unsigned long)(h - 1 - y) * w;
        memset(row, 0, rowbytes);
        for (x = 0; x < w; ++x) {
            unsigned short p = src[x];
            unsigned int r5 = (p >> 11) & 0x1F;
            unsigned int g6 = (p >> 5)  & 0x3F;
            unsigned int b5 =  p        & 0x1F;
            /* 5/6-bit -> 8-bit with replicate-high-bits expansion. */
            row[x*3+0] = (unsigned char)((b5 << 3) | (b5 >> 2)); /* B */
            row[x*3+1] = (unsigned char)((g6 << 2) | (g6 >> 4)); /* G */
            row[x*3+2] = (unsigned char)((r5 << 3) | (r5 >> 2)); /* R */
        }
        fwrite(row, 1, rowbytes, f);
    }
    free(row);
    fclose(f);
    return 1;
}

static unsigned long
parse_which(const char *s)
{
    if (!strcmp(s, "desktop") || !strcmp(s, "1")) return FXDBG_RB_DESKTOP;
    return FXDBG_RB_BACK;   /* default / "back" / "0" */
}

static int
do_readback(HDC hdc, unsigned long which, unsigned long x, unsigned long y,
            unsigned long w, unsigned long h, const char *outpath)
{
    fxdbg_readback_in_t in;
    unsigned short *buf;
    unsigned long need = w * h * 2u;   /* 16bpp 565 */
    int r;

    if (w == 0 || h == 0 || need == 0) {
        fprintf(stderr, "fxdbg: readback rect is empty\n");
        return 0;
    }
    buf = (unsigned short *)calloc(1, need);
    if (!buf) { fprintf(stderr, "fxdbg: OOM\n"); return 0; }

    in.which = which; in.x = x; in.y = y; in.w = w; in.h = h;
    r = ExtEscape(hdc, ESC(FXDBG_READBACK), (int)sizeof(in), (LPCSTR)&in,
                  (int)need, (LPSTR)buf);
    if (r <= 0) {
        fprintf(stderr, "fxdbg: READBACK escape returned %d\n", r);
        free(buf);
        return 0;
    }
    printf("  readback %s (%lu,%lu %lux%lu): %d bytes\n",
           which == FXDBG_RB_DESKTOP ? "desktop" : "back", x, y, w, h, r);
    if (outpath) {
        /* the driver clamps to what fits; only emit whole rows we received. */
        unsigned long rows = (unsigned long)r / (w * 2u);
        if (rows == 0) rows = 1;
        if (rows > h) rows = h;
        if (write_bmp565(outpath, buf, w, rows))
            printf("  wrote %s (%lux%lu, 24bpp BMP)\n", outpath, w, rows);
    }
    free(buf);
    return r > 0;
}

static void
usage(void)
{
    printf(
"fxdbg - fxD3D on-card bring-up ladder driver (M4d)\n"
"usage:\n"
"  fxdbg support                 - QUERYESCSUPPORT for each rung\n"
"  fxdbg probe                   - rung 1: BAR0 status + FIFO + layout\n"
"  fxdbg clear R G B             - rung 2: gb_clear+swap (0..255 each)\n"
"  fxdbg tri                     - rung 3: one gouraud triangle + swap\n"
"  fxdbg tex                     - rung 4: checker texture + quad + swap\n"
"  fxdbg readback WHICH X Y W H [out.bmp]\n"
"                                - rung 5: framebuffer readback (WHICH=back|desktop)\n"
"  fxdbg ladder                  - run rungs 1..5 in order, PASS/FAIL each\n"
"exit code 0 = every requested rung ok and unfaulted.\n");
}

int
main(int argc, char **argv)
{
    HDC hdc;
    int rc = 1;

    if (argc < 2) { usage(); return 2; }

    hdc = open_display_dc();
    if (!hdc) return 3;

    if (!strcmp(argv[1], "support")) {
        static const struct { unsigned long op; const char *n; } rungs[] = {
            { FXDBG_PROBE, "PROBE" }, { FXDBG_CLEAR, "CLEAR" },
            { FXDBG_TRI, "TRI" }, { FXDBG_TEX, "TEX" },
            { FXDBG_READBACK, "READBACK" },
        };
        int i, all = 1;
        for (i = 0; i < 5; ++i) {
            int ok = query_supported(hdc, rungs[i].op);
            printf("  %-8s 0x%04lX  %s\n", rungs[i].n, rungs[i].op,
                   ok ? "supported" : "NOT supported");
            all = all && ok;
        }
        rc = all ? 0 : 1;
    }
    else if (!strcmp(argv[1], "probe")) {
        rc = do_probe(hdc, 1) ? 0 : 1;
    }
    else if (!strcmp(argv[1], "clear")) {
        fxdbg_clear_in_t in;
        if (argc < 5) { usage(); rc = 2; goto done; }
        in.r = strtoul(argv[2], NULL, 0);
        in.g = strtoul(argv[3], NULL, 0);
        in.b = strtoul(argv[4], NULL, 0);
        printf("[rung 2] CLEAR %lu %lu %lu:\n", in.r, in.g, in.b);
        rc = do_status_rung(hdc, FXDBG_CLEAR, &in, (int)sizeof(in), "CLEAR") ? 0 : 1;
    }
    else if (!strcmp(argv[1], "tri")) {
        printf("[rung 3] TRI:\n");
        rc = do_status_rung(hdc, FXDBG_TRI, NULL, 0, "TRI") ? 0 : 1;
    }
    else if (!strcmp(argv[1], "tex")) {
        printf("[rung 4] TEX:\n");
        rc = do_status_rung(hdc, FXDBG_TEX, NULL, 0, "TEX") ? 0 : 1;
    }
    else if (!strcmp(argv[1], "readback")) {
        unsigned long which, x, y, w, h;
        const char *out = NULL;
        if (argc < 7) { usage(); rc = 2; goto done; }
        which = parse_which(argv[2]);
        x = strtoul(argv[3], NULL, 0);
        y = strtoul(argv[4], NULL, 0);
        w = strtoul(argv[5], NULL, 0);
        h = strtoul(argv[6], NULL, 0);
        if (argc >= 8) out = argv[7];
        printf("[rung 5] READBACK:\n");
        rc = do_readback(hdc, which, x, y, w, h, out) ? 0 : 1;
    }
    else if (!strcmp(argv[1], "ladder")) {
        fxdbg_clear_in_t clr;
        int ok = 1, step;
        printf("=== fxD3D bring-up ladder (rungs 1..5) ===\n");

        printf("[rung 1] PROBE:\n");
        step = do_probe(hdc, 1);
        printf("  -> %s\n", step ? "PASS" : "FAIL");
        ok = ok && step;

        clr.r = 0; clr.g = 128; clr.b = 255;
        printf("[rung 2] CLEAR 0 128 255:\n");
        step = do_status_rung(hdc, FXDBG_CLEAR, &clr, (int)sizeof(clr), "CLEAR");
        printf("  -> %s\n", step ? "PASS" : "FAIL");
        ok = ok && step;

        printf("[rung 3] TRI:\n");
        step = do_status_rung(hdc, FXDBG_TRI, NULL, 0, "TRI");
        printf("  -> %s\n", step ? "PASS" : "FAIL");
        ok = ok && step;

        printf("[rung 4] TEX:\n");
        step = do_status_rung(hdc, FXDBG_TEX, NULL, 0, "TEX");
        printf("  -> %s\n", step ? "PASS" : "FAIL");
        ok = ok && step;

        printf("[rung 5] READBACK back 0 0 32 32:\n");
        step = do_readback(hdc, FXDBG_RB_BACK, 0, 0, 32, 32, NULL);
        printf("  -> %s\n", step ? "PASS" : "FAIL");
        ok = ok && step;

        printf("=== ladder %s ===\n", ok ? "ALL PASS" : "FAILED");
        rc = ok ? 0 : 1;
    }
    else {
        usage();
        rc = 2;
    }

done:
    DeleteDC(hdc);
    return rc;
}
