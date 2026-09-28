/* test_vcr_kmd_fbshot.c - TRUE-SOURCE test of
 * voodoo-cleanroom/vcr-kmd/include/vcr_fbshot.h, the decoding behind
 * `vcrctl fbshot` (2026-09-28): read the memory the video processor is
 * scanning out, because a GDI screenshot of a fullscreen Glide/GL/D3D game on
 * our driver photographs the old desktop memory instead.
 *
 * The register values are .124's own, from the flight recorder's MODESET_DONE
 * lines that night: 1280x1024x32@85 = vidProcCfg 000c0081, vidScreenSize
 * 00400500, stride 00001400; Warcraft II's 640x480x8 = 00000081, 001e0280,
 * 00000280. Quake III (4-chip SLI, 1280x960x32) scanned out through the
 * overlay: vidProcCfg 026c0101, stride 00280028, vidCurrOverlayStartAddr
 * 03c3e000; the master's cfgSliLfbCtrl 1e1f0060 (SLI_STEP 406 on silicon).
 *
 * What this pins:
 *   - the format field and bytes per pixel those registers mean;
 *   - a linear offset is y * stride + x, and a TILED one is not (128 x 32
 *     tiles, stride counted in tiles);
 *   - THE OVERLAY FIX: above lfbMemoryConfig's tile begin page memBase1 is a
 *     LINEAR aperture over the tiled memory, and in SLI the chips answer their
 *     own lines through it. A model of that hardware (4 chips, 32-line bands,
 *     tiled, the aperture routing each line to its owner) is filled with a
 *     known frame; the decode must read it back exactly - both of Quake III's
 *     buffers - and the FIRST overlay build's read (raw tile offsets through
 *     the aperture) must come out as the column blocks .124 photographed;
 *   - Glide's own allocation puts colBuffStart0[0] at the captured 03c3e000,
 *     and the lfbMemoryConfig it programs decodes back to that aperture;
 *   - SLI band ownership, the overlay format codes, RGB1555;
 *   - memBase1 is never read while multi-chip AA is live.
 */
#include <stdlib.h>
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_regs.h"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_fbshot.h"

static unsigned fmt_of(unsigned long vpc)
{
    return (unsigned)((vpc & VCR_VPC_DESKTOP_FMT_MASK) >> VCR_VPC_DESKTOP_FMT_SHIFT);
}

TEST(the_124_desktop_registers_decode)
{
    unsigned long vpc = 0x000c0081, ss = 0x00400500, stride = 0x1400;
    CHECK(fmt_of(vpc) == VCR_VPC_FMT_RGB32, "1280x1024x32 is RGB32");
    CHECK(vcr_fb_bytespp(fmt_of(vpc)) == 4, "4 bytes a pixel");
    CHECK((ss & 0xfff) == 1280 && (ss >> 12 & 0xfff) == 1024, "screen size");
    CHECK(!(vpc & VCR_VPC_DESKTOP_TILED_EN), "the desktop is linear");
    CHECK(vcr_fb_pitch(stride, 0) == 5120, "pitch in bytes");
    CHECK(vcr_fb_extent(1280, 1024, 4, stride, 0) == 1023ul * 5120 + 5120, "extent");
    vpc = 0x00000081;
    CHECK(fmt_of(vpc) == VCR_VPC_FMT_PAL8 && vcr_fb_bytespp(fmt_of(vpc)) == 1,
          "Warcraft II's 640x480x8 is palettized");
    CHECK(vcr_fb_extent(640, 480, 1, 0x280, 0) == 479ul * 640 + 640, "8 bpp extent");
}

TEST(the_start_address_keeps_every_bit_a_64mb_chip_needs)
{
    /* .124's desktop (recorder: "primary at 3b00000") and Warcraft II's
     * 640x480x8 primary (3fb5000) */
    CHECK(vcr_fb_start(0x03b00000ul) == 0x03b00000ul, "the desktop");
    CHECK(vcr_fb_start(0x03fb5000ul) == 0x03fb5000ul, "a DirectDraw primary");
    CHECK((0x03b00000ul & 0xfffffful) == 0x00b00000ul && vcr_fb_start(0x03b00000ul) != 0x00b00000ul,
          "the first build's 24-bit mask read texture memory");
}

TEST(linear_offsets_are_y_times_stride_plus_x)
{
    CHECK(vcr_fb_offset(0, 0, 0x1400, 0) == 0, "origin");
    CHECK(vcr_fb_offset(12, 3, 0x1400, 0) == 3 * 5120 + 12, "a pixel");
    CHECK(vcr_fb_offset(12, 3, 0x81400, 0) == 3 * 5120 + 12, "overlay stride bits ignored");
}

TEST(tiled_offsets_walk_128x32_tiles)
{
    /* 10 tiles a row (1280 bytes) */
    unsigned long st = 10;
    CHECK(vcr_fb_pitch(st, 1) == 1280, "stride counts tiles");
    CHECK(vcr_fb_offset(0, 0, st, 1) == 0, "origin");
    CHECK(vcr_fb_offset(127, 0, st, 1) == 127, "end of the first tile's first line");
    CHECK(vcr_fb_offset(128, 0, st, 1) == 4096, "the next tile starts 4 KB on");
    CHECK(vcr_fb_offset(0, 1, st, 1) == 128, "line 1 is 128 bytes into the tile");
    CHECK(vcr_fb_offset(0, 32, st, 1) == 10 * 4096, "line 32 is the next tile row");
    CHECK(vcr_fb_offset(130, 33, st, 1) == (10 + 1) * 4096 + 128 + 2, "both at once");
    /* the linear formula on the same numbers is a different place */
    CHECK(vcr_fb_offset(128, 0, st, 1) != vcr_fb_offset(128, 0, 1280, 0), "tiled != linear");
    CHECK(vcr_fb_extent(640, 480, 2, st, 1) == 15ul * 10 * 4096, "15 tile rows");
    CHECK(vcr_fb_extent(640, 481, 2, st, 1) == 16ul * 10 * 4096, "a partial tile row counts");
}

TEST(pixels_decode_to_rgb)
{
    unsigned char p565[2] = { 0x00, 0xf8 };            /* pure red */
    unsigned char g565[2] = { 0xe0, 0x07 };            /* pure green */
    unsigned char w565[2] = { 0xff, 0xff };
    unsigned char r1555[2] = { 0x00, 0x7c };           /* 1555 red */
    unsigned char g1555[2] = { 0xe0, 0x03 };           /* 1555 green */
    unsigned char w1555[2] = { 0xff, 0x7f };
    unsigned char p32[4] = { 0x33, 0x22, 0x11, 0x00 }; /* BGRX */
    unsigned char p24[3] = { 0x33, 0x22, 0x11 };
    unsigned char i8[1] = { 7 };
    unsigned long clut[256];
    int i;
    for (i = 0; i < 256; i++)
        clut[i] = 0x010203ul * (unsigned long)i;
    CHECK(vcr_fb_rgb(1, p565, NULL) == 0xff0000ul, "565 red");
    CHECK(vcr_fb_rgb(1, g565, NULL) == 0x00ff00ul, "565 green");
    CHECK(vcr_fb_rgb(1, w565, NULL) == 0xfffffful, "565 white is white, not 0xf8fcf8");
    CHECK(vcr_fb_rgb(4, r1555, NULL) == 0xff0000ul, "1555 red");
    CHECK(vcr_fb_rgb(4, g1555, NULL) == 0x00ff00ul, "1555 green");
    CHECK(vcr_fb_rgb(4, w1555, NULL) == 0xfffffful, "1555 white");
    CHECK(vcr_fb_rgb(1, r1555, NULL) != 0xff0000ul, "1555 read as 565 is not red: the code matters");
    CHECK(vcr_fb_rgb(3, p32, NULL) == 0x112233ul, "32 bpp is BGRX in memory");
    CHECK(vcr_fb_rgb(2, p24, NULL) == 0x112233ul, "24 bpp");
    CHECK(vcr_fb_rgb(0, i8, clut) == 0x070e15ul, "8 bpp goes through the CLUT");
    CHECK(vcr_fb_rgb(0, i8, NULL) == 0x070707ul, "no CLUT: grey ramp, never black");
    /* desktop code 4 is RGB1555 on a VSA-100 (h3defs.h SST_DESKTOP_PIXEL_RGB1555U) */
    CHECK(vcr_fb_bytespp(4) == 2, "RGB1555 is 2 bytes");
    CHECK(vcr_fb_bytespp(5) == 0 && vcr_fb_rgb(5, p32, NULL) == 0, "an unknown format is refused");
    CHECK(vcr_fb_bytespp(7) == 0, "7 (the YUV overlay stand-in) is refused");
}

TEST(the_overlay_layer_of_a_glide_game)
{
    /* Quake III, 4-chip SLI, 1280x960x32: vidProcCfg 026c0101, stride 00280028 */
    unsigned long vpc = 0x026c0101ul, stride = 0x00280028ul;
    unsigned long ost = vcr_fb_overlay_stride(stride);
    CHECK(!(vpc & VCR_VPC_DESKTOP_EN) && (vpc & VCR_VPC_OVERLAY_EN), "desktop off, overlay on");
    CHECK((vpc & VCR_VPC_OVERLAY_TILED_EN) != 0, "the overlay is tiled");
    CHECK(ost == 0x28 && vcr_fb_pitch(ost, 1) == 5120, "40 tiles = 5120 bytes");
    CHECK(vcr_fb_overlay_fmt(vpc) == 3, "overlay format RGB32U (bits 23:21 = 3)");
    CHECK(vcr_fb_overlay_bytespp(5120, 1280) == 4, "the pitch agrees: 32 bpp");
    CHECK(vcr_fb_overlay_bytespp(2560, 1280) == 2, "16 bpp");
    CHECK(vcr_fb_overlay_bytespp(1664, 800) == 2, "800 px x 2 = 1600 bytes, rounded up to 13 tiles");
    CHECK(vcr_fb_overlay_bytespp(3840, 1280) == 0, "24 bpp is not an overlay format here: refuse");
    CHECK(vcr_fb_overlay_bytespp(5120, 0) == 0, "no width");
    /* the overlay format codes (h3defs.h SST_OVERLAY_PIXEL_*) */
    CHECK(vcr_fb_overlay_fmt(0ul << 21) == 4 && vcr_fb_overlay_fmt(2ul << 21) == 4, "1555D/1555U");
    CHECK(vcr_fb_overlay_fmt(1ul << 21) == 1 && vcr_fb_overlay_fmt(7ul << 21) == 1, "565U/565D");
    CHECK(vcr_fb_bytespp(vcr_fb_overlay_fmt(4ul << 21)) == 0 &&
          vcr_fb_bytespp(vcr_fb_overlay_fmt(5ul << 21)) == 0 &&
          vcr_fb_bytespp(vcr_fb_overlay_fmt(6ul << 21)) == 0, "YUV overlays are refused");
}

/* ---- Glide's allocation (minihwc.c hwcAllocBuffers / calcBuffer*) --------------
 * 64 MB a chip, one aux buffer, two colour buffers, top down; colour buffers
 * on even pages, aux on odd. Per chip yres = 960 >> log2(4) = 240, rounded up
 * to whole 32-line bands (gsst.c: 960 >= 768 -> band log2 5) = 256 lines. */
static void glide_alloc(unsigned long mem, unsigned long stride_bytes, unsigned long lines,
                        unsigned long col[2], unsigned long *aux)
{
    unsigned long size = stride_bytes * lines, a;
    int i;
    a = mem - size;
    if (!(a & 0x1000))
        a -= 0x1000;
    *aux = a;
    for (i = 1; i >= 0; i--) {
        a -= size;
        if (a & 0x1000)
            a -= 0x1000;
        col[i] = a;
    }
}

TEST(glides_allocation_puts_the_front_buffer_where_124_scanned_it_out)
{
    unsigned long col[2], aux, per_chip = 960 >> 2, bands = (per_chip + 31) / 32, lmc;
    glide_alloc(64ul << 20, 5120, bands * 32, col, &aux);
    CHECK_EQ_U(bands * 32, 256);
    CHECK_EQ_U(col[0], 0x03c3e000ul);     /* = the captured vidCurrOverlayStartAddr */
    CHECK_EQ_U(col[1], 0x03d7e000ul);
    CHECK_EQ_U(aux, 0x03ebf000ul);
    /* hwcInitVideo: lfbMemoryConfig = MUNGE(colBuffStart0[0] >> 12) | 8K | tiles << 16 */
    lmc = (((col[0] >> 12) & 0x1fff) | ((((col[0] >> 12) & 0x6000) >> 13) << 23)) |
          (3ul << 13) | (40ul << 16);
    CHECK_EQ_U(lmc, 0x00a87c3eul);        /* what .124 should read back during Quake III */
    CHECK_EQ_U(vcr_fb_ap_base(lmc), col[0]);
    CHECK_EQ_U(vcr_fb_ap_lfb_stride(lmc), 8192);
    CHECK_EQ_U(vcr_fb_ap_tile_stride(lmc), 40);
    /* our kernel's linear-desktop value and the power-up value (both on .124) */
    CHECK_EQ_U(vcr_fb_ap_base(0x01803ffful), 0x07fff000ul);
    CHECK_EQ_U(vcr_fb_ap_lfb_stride(0x01803ffful), 2048);
    CHECK_EQ_U(vcr_fb_ap_base(0x00003ffful), 0x01fff000ul);
    CHECK_EQ_U(vcr_fb_ap_lfb_stride(5ul << 13), 0);   /* a reserved code */
}

TEST(sli_bands_which_lines_each_chip_holds)
{
    /* cfgSliLfbCtrl as vcrmp_sli.c writes it for 4-way, 32-line bands (and as
     * .124 logged it): render 0x60, compare c << 5, scan 0x1f, log2 2, W/R en */
    unsigned long ctrl[4] = { 0x1e1f0060ul, 0x1e1f2060ul, 0x1e1f4060ul, 0x1e1f6060ul };
    unsigned long y, n[4] = { 0, 0, 0, 0 };
    int c;
    CHECK(vcr_fb_sli_shift(ctrl[0]) == 2, "4 units");
    CHECK((ctrl[0] & VCR_FB_SLILFB_READ_EN) != 0, "SLI read on");
    CHECK(vcr_fb_sli_owns(0, ctrl[0]) && vcr_fb_sli_owns(31, ctrl[0]), "chip 0: 0-31");
    CHECK(!vcr_fb_sli_owns(32, ctrl[0]) && vcr_fb_sli_owns(32, ctrl[1]), "32 is chip 1's");
    CHECK(vcr_fb_sli_owns(128, ctrl[0]) && vcr_fb_sli_owns(927, ctrl[0]), "chip 0: 128-159 ... 896-927");
    CHECK(vcr_fb_sli_owns(959, ctrl[1]), "the last line is chip 1's");
    CHECK_EQ_U(vcr_fb_sli_local_line(128, ctrl[0]), 32);
    CHECK_EQ_U(vcr_fb_sli_local_line(159, ctrl[0]), 63);
    CHECK_EQ_U(vcr_fb_sli_local_line(959, ctrl[1]), 255);
    for (y = 0; y < 960; y++)
        for (c = 0; c < 4; c++)
            if (vcr_fb_sli_owns(y, ctrl[c]))
                n[c]++;
    CHECK(n[0] == 256 && n[1] == 256 && n[2] == 224 && n[3] == 224,
          "30 bands: chips 0 and 1 hold 8, chips 2 and 3 hold 7");
}

/* ---- a model of the hardware --------------------------------------------------
 * NCHIP chips, each with its own memory; a frame lives in tiled buffers with
 * each chip holding the lines it owns, packed (vcr_fb_sli_owns/local_line).
 * mb1_read() is what a CPU read of the master's memBase1 returns: raw memory
 * of the master below the aperture's begin page; above it, byte (x', y') of
 * the aperture's linear picture, answered by the chip that owns line y' and
 * converted to its tile (a line longer than the tile stride runs on into the
 * next tile row, as the conversion's arithmetic does). */
#define MEM_LO      0x03a00000ul
#define MEM_HI      0x04000000ul
typedef struct model {
    int units;                  /* SLI units; 1 = no SLI */
    unsigned long ctrl[4];      /* cfgSliLfbCtrl per chip */
    vcr_fb_aperture ap;
    unsigned char *mem[4];
} model;

static unsigned long pix_value(int buf, unsigned long x, unsigned long y)
{
    return 0x80000000ul | (unsigned long)buf << 28 | y << 12 | x;
}

static unsigned char *chip_byte(model *m, int c, unsigned long phys)
{
    if (phys < MEM_LO || phys >= MEM_HI)
        return NULL;
    return m->mem[c] + (phys - MEM_LO);
}

static int owner(const model *m, unsigned long y)
{
    int c;
    if (m->units == 1)
        return 0;
    for (c = 0; c < m->units; c++)
        if (vcr_fb_sli_owns(y, m->ctrl[c]))
            return c;
    return -1;
}

static unsigned long local_line(const model *m, unsigned long y)
{
    return m->units == 1 ? y : vcr_fb_sli_local_line(y, m->ctrl[0]);
}

/* put frame `buf` (w x h, bpp) into the tiled buffer at `phys`, stride `ts` tiles */
static void fill_tiled(model *m, int buf, unsigned long phys, unsigned long ts, unsigned w,
                       unsigned h, unsigned bpp)
{
    unsigned long x, y;
    unsigned k;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned long v = pix_value(buf, x, y);
            unsigned long a = phys + vcr_fb_offset(x * bpp, local_line(m, y), ts, 1);
            for (k = 0; k < bpp; k++) {
                unsigned char *p = chip_byte(m, owner(m, y), a + k);
                if (p)
                    *p = (unsigned char)(v >> (8 * k));
            }
        }
}

static unsigned char mb1_read(const model *m, unsigned long o)
{
    unsigned long a, yp, xp, yl, tile, phys;
    int c;
    unsigned char *p;
    if (o < m->ap.base) {
        p = chip_byte((model *)m, 0, o);
        return p ? *p : 0xee;
    }
    a = o - m->ap.base;
    yp = a / m->ap.lfb_stride;
    xp = a % m->ap.lfb_stride;
    c = owner(m, yp);
    yl = local_line(m, yp);
    tile = (yl / 32) * m->ap.tile_stride + xp / 128;
    phys = m->ap.base + tile * 4096 + (yl % 32) * 128 + xp % 128;
    p = c < 0 ? NULL : chip_byte((model *)m, c, phys);
    return p ? *p : 0xee;
}

static unsigned long mb1_pixel(const model *m, unsigned long o, unsigned bpp)
{
    unsigned long v = 0;
    unsigned k;
    for (k = 0; k < bpp; k++)
        v |= (unsigned long)mb1_read(m, o + k) << (8 * k);
    return v;
}

static unsigned long pixel_mask(unsigned bpp)
{
    return bpp >= 4 ? 0xfffffffful : (1ul << (8 * bpp)) - 1;
}

/* the whole frame through a plan, as vcrctl reads it: mismatching pixels */
static unsigned long read_back_mismatches(const model *m, const vcr_fb_plan *p, int buf)
{
    unsigned long y, xb, bad = 0;
    for (y = 0; y < p->l.h; y++)
        for (xb = 0; xb < p->rowbytes;) {
            unsigned long run, off = vcr_fb_plan_addr(p, xb, y, &run), i;
            for (i = 0; i < run; i += p->l.bpp)
                if (mb1_pixel(m, off + i, p->l.bpp) !=
                    (pix_value(buf, (xb + i) / p->l.bpp, y) & pixel_mask(p->l.bpp)))
                    bad++;
            xb += run;
        }
    return bad;
}

static void model_init(model *m, int units, unsigned long lmc)
{
    int c;
    memset(m, 0, sizeof *m);
    m->units = units;
    for (c = 0; c < 4; c++) {
        m->mem[c] = (unsigned char *)calloc(MEM_HI - MEM_LO, 1);
        m->ctrl[c] = units == 4 ? 0x1e1f0060ul | (unsigned long)c << 13 : 0;
    }
    m->ap.base = vcr_fb_ap_base(lmc);
    m->ap.lfb_stride = vcr_fb_ap_lfb_stride(lmc);
    m->ap.tile_stride = vcr_fb_ap_tile_stride(lmc);
    m->ap.sli_shift = units == 4 ? vcr_fb_sli_shift(m->ctrl[0]) : 0;
}

static void model_free(model *m)
{
    int c;
    for (c = 0; c < 4; c++)
        free(m->mem[c]);
}

TEST(quake3_in_4chip_sli_reads_back_whole_through_the_aperture)
{
    model m;
    vcr_fb_layer l;
    vcr_fb_plan p;
    unsigned long bad;
    model_init(&m, 4, 0x00a87c3eul);
    CHECK(m.mem[3] != NULL, "model memory");
    fill_tiled(&m, 0, 0x03c3e000ul, 40, 1280, 960, 4);
    fill_tiled(&m, 1, 0x03d7e000ul, 40, 1280, 960, 4);

    /* the registers .124 read: vidCurrOverlayStartAddr 03c3e000, stride 0x28 */
    memset(&l, 0, sizeof l);
    l.start = 0x03c3e000ul;
    l.stride = vcr_fb_overlay_stride(0x00280028ul);
    l.tiled = 1;
    l.w = 1280;
    l.h = 960;
    l.bpp = vcr_fb_bytespp(vcr_fb_overlay_fmt(0x026c0101ul));
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &m.ap, 128ul << 20), VCR_FB_M_APERTURE);
    CHECK_EQ_U(p.first, 0x03c3e000ul);
    CHECK_EQ_U(p.line, 8192);
    CHECK_EQ_U(p.end, 0x03c3e000ul + 959ul * 8192 + 5120);
    bad = read_back_mismatches(&m, &p, 0);
    CHECK_EQ_U(bad, 0);                   /* every band of every chip, in place */

    /* the other buffer, after a flip: its chip-local line 256 is aperture line
     * 1024 (hwcBufferLfbAddr's << log2(units)), not 256 */
    l.start = 0x03d7e000ul;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &m.ap, 128ul << 20), VCR_FB_M_APERTURE);
    CHECK_EQ_U(p.first, 0x03c3e000ul + 1024ul * 8192);
    CHECK_EQ_U(read_back_mismatches(&m, &p, 1), 0);
    /* memBase1 decodes 2 x 64 MB: buffer 1 ends at 04bbd400, inside it */
    CHECK_EQ_U(p.end, 0x03c3e000ul + (1024ul + 959) * 8192 + 5120);
    CHECK(p.end < (128ul << 20), "inside the decode");
    {
        /* without the SLI shift the "second buffer" is the first one's lines */
        vcr_fb_aperture noshift = m.ap;
        noshift.sli_shift = 0;
        vcr_fb_plan_make(&p, &l, &noshift, 0);
        CHECK(read_back_mismatches(&m, &p, 1) > 1000000ul, "the shift is what finds buffer 1");
    }
    l.start = 0x03c3e000ul;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &m.ap, 0x03c3e000ul + 4096), VCR_FB_R_RANGE);
    model_free(&m);
}

TEST(the_first_overlay_read_is_the_column_blocks_124_photographed)
{
    /* The first overlay build read start + (raw tiled offset) through the
     * same memBase1. Its pixel (x, y) is aperture byte (tile col, row, line)
     * of an 8 KB-line picture: a 4 KB tile is half a line. */
    model m;
    unsigned long start = 0x03c3e000ul, st = 0x28;
    model_init(&m, 4, 0x00a87c3eul);
    fill_tiled(&m, 0, start, 40, 1280, 960, 4);
#define OLD(x, y) mb1_pixel(&m, start + vcr_fb_offset((x) * 4ul, (y), st, 1), 4)
    CHECK(OLD(0, 0) == pix_value(0, 0, 0), "the origin is the origin");
    CHECK(OLD(0, 1) == pix_value(0, 32, 0), "line 1 of the picture is line 0, 32 px on");
    CHECK(OLD(32, 0) == pix_value(0, 1024, 0), "tile column 1 is line 0 from pixel 1024");
    CHECK(OLD(64, 0) == pix_value(0, 0, 1), "tile column 2 is screen line 1");
    CHECK(OLD(32, 8) != pix_value(0, 32, 8), "the odd columns' lower lines run past the line");
    {
        unsigned long y, x, same = 0;
        for (y = 0; y < 960; y += 7)
            for (x = 0; x < 1280; x += 5)
                same += OLD(x, y) == pix_value(0, x, y);
        CHECK(same < 400, "the old read is not the frame");
    }
#undef OLD
    model_free(&m);
}

TEST(one_chip_glide_and_a_raw_tiled_buffer_below_the_aperture)
{
    model m;
    vcr_fb_layer l;
    vcr_fb_plan p;
    /* one chip, 640x480x16, 10 tiles a row, aperture at 0x3e00000 (8K lines) */
    unsigned long lmc = ((0x3e00ul & 0x1fff) | (((0x3e00ul & 0x6000) >> 13) << 23)) | (3ul << 13) |
                        (10ul << 16);
    model_init(&m, 1, lmc);
    CHECK_EQ_U(m.ap.base, 0x03e00000ul);
    fill_tiled(&m, 0, 0x03e00000ul, 10, 640, 480, 2);   /* in the aperture */
    fill_tiled(&m, 1, 0x03b00000ul, 10, 640, 480, 2);   /* raw, below it */
    memset(&l, 0, sizeof l);
    l.stride = 10;
    l.tiled = 1;
    l.w = 640;
    l.h = 480;
    l.bpp = 2;
    l.start = 0x03e00000ul;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &m.ap, 0), VCR_FB_M_APERTURE);
    CHECK_EQ_U(read_back_mismatches(&m, &p, 0), 0);
    l.start = 0x03b00000ul;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &m.ap, 0), VCR_FB_M_TILED);
    CHECK_EQ_U(read_back_mismatches(&m, &p, 1), 0);
    /* a raw tiled buffer that runs into the aperture is refused, not guessed */
    l.start = 0x03e00000ul - 4 * 10 * 4096ul;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &m.ap, 0), VCR_FB_R_STRADDLE);
    model_free(&m);
}

TEST(a_linear_surface_inside_the_aperture)
{
    /* a DirectDraw primary above Glide's begin page (no mode set in between):
     * read raw through the aperture's inverse - one chip only */
    model m;
    vcr_fb_layer l;
    vcr_fb_plan p;
    unsigned long lmc = ((0x3c3eul & 0x1fff) | (((0x3c3eul & 0x6000) >> 13) << 23)) |
                        (3ul << 13) | (40ul << 16), x, y, bad = 0;
    model_init(&m, 1, lmc);
    for (y = 0; y < 480; y++)
        for (x = 0; x < 640; x++)
            *chip_byte(&m, 0, 0x03fb5000ul + y * 640 + x) = (unsigned char)(x * 7 + y * 3);
    memset(&l, 0, sizeof l);
    l.start = 0x03fb5000ul;
    l.stride = 640;
    l.w = 640;
    l.h = 480;
    l.bpp = 1;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &m.ap, 0), VCR_FB_M_LINEAR_AP);
    for (y = 0; y < 480; y++)
        for (x = 0; x < 640;) {
            unsigned long run, off = vcr_fb_plan_addr(&p, x, y, &run), i;
            CHECK(run >= 1 && run <= 128, "chunks never cross a 128-byte group");
            for (i = 0; i < run; i++)
                bad += mb1_read(&m, off + i) != (unsigned char)((x + i) * 7 + y * 3);
            x += run;
        }
    CHECK_EQ_U(bad, 0);
    CHECK(p.end >= vcr_fb_ap_addr(0x03fb5000ul + 479ul * 640 + 639, &m.ap) + 1, "end bounds the read");
    /* the same surface under SLI has no single chip's raw memory to point at */
    m.ap.sli_shift = 2;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &m.ap, 0), VCR_FB_R_SLI_LINEAR);
    model_free(&m);
}

TEST(the_verified_desktop_reads_stay_raw)
{
    /* our kernel's linear desktop pushes the aperture past memory: the reads
     * that matched GDI (mean abs diff 3.51) are unchanged */
    vcr_fb_aperture ap;
    vcr_fb_layer l;
    vcr_fb_plan p;
    unsigned long run;
    memset(&ap, 0, sizeof ap);
    ap.base = vcr_fb_ap_base(0x01803ffful);
    ap.lfb_stride = vcr_fb_ap_lfb_stride(0x01803ffful);
    ap.tile_stride = vcr_fb_ap_tile_stride(0x01803ffful);
    memset(&l, 0, sizeof l);
    l.start = 0x03b00000ul;
    l.stride = 0x1400;
    l.w = 1280;
    l.h = 1024;
    l.bpp = 4;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 128ul << 20), VCR_FB_M_LINEAR);
    CHECK_EQ_U(vcr_fb_plan_addr(&p, 12, 3, &run), 0x03b00000ul + 3 * 5120 + 12);
    CHECK_EQ_U(run, 5120 - 12);
    l.start = 0x03fb5000ul;             /* Warcraft II's primary */
    l.stride = 0x280;
    l.w = 640;
    l.h = 480;
    l.bpp = 1;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 128ul << 20), VCR_FB_M_LINEAR);
    CHECK_EQ_U(p.end, 0x04000000ul);
}

TEST(what_the_plan_refuses)
{
    vcr_fb_aperture ap;
    vcr_fb_layer l;
    vcr_fb_plan p;
    memset(&ap, 0, sizeof ap);
    ap.base = 0x03c3e000ul;
    ap.lfb_stride = 8192;
    ap.tile_stride = 40;
    memset(&l, 0, sizeof l);
    l.start = 0x03c3e000ul;
    l.stride = 0x28;
    l.tiled = 1;
    l.w = 1280;
    l.h = 960;
    l.bpp = 4;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 0), VCR_FB_M_APERTURE);
    l.stride = 0x14;                    /* half a line of tiles */
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 0), VCR_FB_R_FORMAT);
    l.stride = 0x29;                    /* the aperture converts 40, the layer is 41 */
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 0), VCR_FB_R_AP_STRIDE);
    l.stride = 0x28;
    ap.lfb_stride = 4096;               /* 5120-byte lines do not fit 4 KB */
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 0), VCR_FB_R_AP_LINE);
    ap.lfb_stride = 0;                  /* a reserved stride code */
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 0), VCR_FB_R_AP_CODE);
    ap.lfb_stride = 8192;
    l.bpp = 0;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 0), VCR_FB_R_FORMAT);
    CHECK(vcr_fb_method_name(VCR_FB_R_AP_STRIDE)[0] == 'r', "every refusal has a name");
}

TEST(membase1_is_not_read_while_multichip_aa_is_live)
{
    /* one chip: Glide's rule 11 lets a read through, and so do we */
    CHECK_EQ_I(vcr_fb_mb1_gate(0, 0, 0, 0), 0);
    CHECK_EQ_I(vcr_fb_mb1_gate(1, 0, 0, 0x0c000000ul), 0);
    /* Quake III's 4-way SLI alone: READ_EN on, cfgAALfbCtrl 0 */
    CHECK_EQ_I(vcr_fb_mb1_gate(4, 1, 0x1e1f0060ul, 0), 0);
    /* any AA enable writes the AA LFB CPU/dispatch write enables */
    CHECK_EQ_I(vcr_fb_mb1_gate(4, 1, 0x1e1f0060ul, 0x0c000000ul), VCR_FB_G_AA);
    CHECK_EQ_I(vcr_fb_mb1_gate(4, 1, 0, 0x1c000000ul), VCR_FB_G_AA);        /* AA alone */
    CHECK_EQ_I(vcr_fb_mb1_gate(2, 1, 0x1c1f0020ul, 0x10000000ul), VCR_FB_G_AA);
    /* a live session we cannot read = refuse */
    CHECK_EQ_I(vcr_fb_mb1_gate(4, 0, 0, 0), VCR_FB_G_CFG_UNKNOWN);
    /* SLI with the hardware's SLI read off, or neither SLI nor AA */
    CHECK_EQ_I(vcr_fb_mb1_gate(4, 1, 0x0e1f0060ul, 0), VCR_FB_G_SLI_NOREAD);
    CHECK_EQ_I(vcr_fb_mb1_gate(4, 1, 0, 0), VCR_FB_G_SLI_NOREAD);
    CHECK(vcr_fb_gate_name(VCR_FB_G_AA)[0] == 'r', "a refusal says so");
}

/* Integration review (2026-09-28), (e): on a multi-chip board the master's
 * SLI/AA registers decide, not the kernel's session count. The old gate let
 * a read through whenever OUR kernel had no session (sli_chips 0 or 1) -
 * even with the AA write/read enables set by something else. */
TEST(the_board_not_the_kernels_count_decides_the_gate)
{
    /* the hole: .124 (4 chips), the kernel's count 0, AA enables set */
    CHECK_EQ_I(vcr_fb_mb1_gate(0, 1, 0, 0x1c000000ul), 0);          /* old form: let through */
    CHECK_EQ_I(vcr_fb_mb1_gate_board(4, 0, 1, 0, 0x1c000000ul), VCR_FB_G_AA);
    CHECK_EQ_I(vcr_fb_mb1_gate_board(4, 1, 1, 0, 0x04000000ul), VCR_FB_G_AA);  /* CPU write en */
    CHECK_EQ_I(vcr_fb_mb1_gate_board(4, 0, 1, 0, 0x10000000ul), VCR_FB_G_AA);  /* read en alone */
    CHECK_EQ_I(vcr_fb_mb1_gate_board(2, 0, 1, 0, 0x08000000ul), VCR_FB_G_AA);  /* a V5 5500 */
    /* a multi-chip board whose registers cannot be read: refuse */
    CHECK_EQ_I(vcr_fb_mb1_gate_board(4, 0, 0, 0, 0), VCR_FB_G_CFG_UNKNOWN);
    /* the desktop on .124 with no session: both registers 0 - read */
    CHECK_EQ_I(vcr_fb_mb1_gate_board(4, 0, 1, 0, 0), 0);
    /* SLI units dealt that the kernel does not know about, with no SLI read */
    CHECK_EQ_I(vcr_fb_mb1_gate_board(4, 0, 1, 0x0e1f0060ul, 0), VCR_FB_G_SLI_NOREAD);
    /* ...and Quake III's SLI as logged, READ_EN on: read */
    CHECK_EQ_I(vcr_fb_mb1_gate_board(4, 4, 1, 0x1e1f0060ul, 0), 0);
    /* a single-chip card is never asked (Voodoo3, one-chip VSA-100) */
    CHECK_EQ_I(vcr_fb_mb1_gate_board(1, 0, 0, 0, 0xfffffffful), 0);
    /* the original form keeps every answer it had */
    CHECK_EQ_I(vcr_fb_mb1_gate(4, 1, 0x1e1f0060ul, 0x0c000000ul), VCR_FB_G_AA);
    CHECK_EQ_I(vcr_fb_mb1_gate(4, 1, 0, 0), VCR_FB_G_SLI_NOREAD);
}

/* (d): the gate is asked again during the read - before line 0, at every
 * flip followed and every 64 lines - and the read stops the moment it says
 * no. A model of vcrctl's loop over 960 lines with AA switched on after
 * line 100 stops at line 128, having read nothing past it; switched on at a
 * flip, it stops AT the flip. */
static unsigned long run_reader(unsigned long h, unsigned long aa_on_at, unsigned long flip_at,
                                unsigned long *lines_read, int *rechecks)
{
    unsigned long y, aa;
    *lines_read = 0;
    *rechecks = 0;
    for (y = 0; y < h; y++) {
        int flipped = y == flip_at;
        if (vcr_fb_recheck_due(y, flipped)) {
            (*rechecks)++;
            aa = y >= aa_on_at ? 0x0c000000ul : 0;
            if (vcr_fb_mb1_recheck(4, 4, 1, 0x1e1f0060ul, aa, 2))
                return y;
        }
        (*lines_read)++;            /* the memBase1 read of line y */
    }
    return h;
}

TEST(a_read_stops_at_the_next_recheck_after_aa_goes_live)
{
    unsigned long lines, stop;
    int n;
    CHECK_EQ_U(VCR_FB_RECHECK_LINES, 64);
    CHECK(vcr_fb_recheck_due(0, 0), "before the first line");
    CHECK(vcr_fb_recheck_due(64, 0) && vcr_fb_recheck_due(128, 0), "every 64 lines");
    CHECK(!vcr_fb_recheck_due(63, 0) && !vcr_fb_recheck_due(65, 0), "not in between");
    CHECK(vcr_fb_recheck_due(77, 1), "at a flip");
    /* no AA: the whole frame, 15 rechecks for 960 lines */
    stop = run_reader(960, 100000, 100000, &lines, &n);
    CHECK_EQ_U(stop, 960);
    CHECK_EQ_U(lines, 960);
    CHECK_EQ_I(n, 15);
    /* AA after line 100: stopped at 128, lines 0-127 read, none after */
    stop = run_reader(960, 101, 100000, &lines, &n);
    CHECK_EQ_U(stop, 128);
    CHECK_EQ_U(lines, 128);
    /* AA with a flip at line 77: stopped at the flip */
    stop = run_reader(960, 77, 77, &lines, &n);
    CHECK_EQ_U(stop, 77);
    CHECK_EQ_U(lines, 77);
    /* AA already live: nothing read */
    stop = run_reader(960, 0, 100000, &lines, &n);
    CHECK_EQ_U(stop, 0);
    CHECK_EQ_U(lines, 0);
    /* the SLI units changing under the read also stop it */
    CHECK_EQ_I(vcr_fb_mb1_recheck(4, 4, 1, 0x1d1f0020ul, 0, 2), VCR_FB_G_SLI_CHANGED);
    CHECK_EQ_I(vcr_fb_mb1_recheck(4, 4, 1, 0x1e1f0060ul, 0, 2), 0);
    CHECK_EQ_I(vcr_fb_mb1_recheck(4, 4, 0, 0x1e1f0060ul, 0, 2), VCR_FB_G_CFG_UNKNOWN);
    CHECK_EQ_I(vcr_fb_mb1_recheck(1, 0, 0, 0, 0, 0), 0);             /* one chip: never */
    CHECK(vcr_fb_gate_name(VCR_FB_G_SLI_CHANGED)[0] == 's', "a stop says so");
}

/* (f): under SLI the master's raw memory is its own bands only - a raw read
 * (below the aperture) is not the frame, so it is refused, not guessed. */
TEST(raw_reads_are_refused_under_sli)
{
    vcr_fb_aperture ap;
    vcr_fb_layer l;
    vcr_fb_plan p;
    memset(&ap, 0, sizeof ap);
    ap.base = 0x03c3e000ul;
    ap.lfb_stride = 8192;
    ap.tile_stride = 40;
    memset(&l, 0, sizeof l);
    l.start = 0x03000000ul;                 /* below the aperture */
    l.stride = 0x28;
    l.tiled = 1;
    l.w = 1280;
    l.h = 960;
    l.bpp = 4;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 0), VCR_FB_M_TILED);
    ap.sli_shift = 2;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 0), VCR_FB_R_SLI_RAW);
    l.tiled = 0;
    l.stride = 5120;
    l.start = 0x02000000ul;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 0), VCR_FB_R_SLI_RAW);
    ap.sli_shift = 0;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 0), VCR_FB_M_LINEAR);
    /* the aperture read under SLI is unchanged */
    ap.sli_shift = 2;
    l.start = 0x03c3e000ul;
    l.stride = 0x28;
    l.tiled = 1;
    CHECK_EQ_I(vcr_fb_plan_make(&p, &l, &ap, 0), VCR_FB_M_APERTURE);
    CHECK(vcr_fb_method_name(VCR_FB_R_SLI_RAW)[0] == 'r', "a refusal has a name");
}

MUNIT_MAIN("vcr-kmd fbshot decoding (include/vcr_fbshot.h)", {
    RUN(the_124_desktop_registers_decode);
    RUN(the_start_address_keeps_every_bit_a_64mb_chip_needs);
    RUN(linear_offsets_are_y_times_stride_plus_x);
    RUN(tiled_offsets_walk_128x32_tiles);
    RUN(pixels_decode_to_rgb);
    RUN(the_overlay_layer_of_a_glide_game);
    RUN(glides_allocation_puts_the_front_buffer_where_124_scanned_it_out);
    RUN(sli_bands_which_lines_each_chip_holds);
    RUN(quake3_in_4chip_sli_reads_back_whole_through_the_aperture);
    RUN(the_first_overlay_read_is_the_column_blocks_124_photographed);
    RUN(one_chip_glide_and_a_raw_tiled_buffer_below_the_aperture);
    RUN(a_linear_surface_inside_the_aperture);
    RUN(the_verified_desktop_reads_stay_raw);
    RUN(what_the_plan_refuses);
    RUN(membase1_is_not_read_while_multichip_aa_is_live);
    RUN(the_board_not_the_kernels_count_decides_the_gate);
    RUN(a_read_stops_at_the_next_recheck_after_aa_goes_live);
    RUN(raw_reads_are_refused_under_sli);
})
