/*
 * vcr_sliaa.h - `vcrctl sliaa`: the argument parser, the safety gates and the
 * request builder, as pure C (no Win32, no CRT), so the host test compiles
 * the code the tool runs (tests/native/test_vcr_kmd_sli.c).
 *
 *   vcrctl sliaa <n> <sli> <aa> <high> <analog> [nlines bpp tileMark col depthlo depthhi]
 *   vcrctl sliaa off
 *   ... --i-am-at-the-box      REQUIRED for every enable (SLI or AA)
 *   ... --force-desktop-pll    an AA enable on a desktop that is not in 2x mode
 *
 * It sends HWCEXT_SLI_AA_REQUEST through ExtEscape exactly as Glide does
 * (minihwc.c, the HWC_MINIVDD_HACK block): the same payload, the same escape,
 * and - on our driver - the same kernel path, policy and all. So it is the
 * kernel-only probe of an SLI/AA configuration: no grSstWinOpen, no LFB, no
 * DAC - just what the kernel programs, which config-space reads (`vcrctl
 * pci`) and Diag\SliAAState then show.
 *
 * AN ENABLE REPROGRAMS THE SCAN-OUT: the slaves' video PLL follows the
 * master's sync clock, the V5 6000's external clock is set, the video is
 * re-muxed. So the tool treats it as a mode switch, the way Glide's open is
 * one (vcrctl.c cmd_sliaa): it is paced by vcr_pace.h, and it runs under
 * HWCSETEXCLUSIVE - the 2D engine drained and GDI parked, as under Glide -
 * which `sliaa off` gives back (HWCRLSEXCLUSIVE restores the desktop mode).
 *
 * THE GATES, all before the tool opens any escape - a refused request sends
 * nothing:
 *   - EVERY enable needs --i-am-at-the-box. Every AA configuration tried on
 *     the V5 6000 froze .124 hard (2026-09-26); an SLI enable reprograms the
 *     same clocks. The kernel refuses AA unless Diag\SliAA = 1; this is
 *     defence in depth, so neither a stray registry value nor a copied
 *     command line alone is enough. `off` needs no flag (it is the way back).
 *   - A shape with no video-mux branch (vcr_sli_combo_ok, the kernel's own
 *     rule) is refused here too: the tool is for ANY HWCEXT driver, and a
 *     vendor kernel has no such refusal - AmigaMerlin's froze on the {4,0,1,0,1}
 *     that Glide sends for cfg 1 on the 6000.
 *   - An AA enable divides the video clock (cfgVideoCtrl0 DIVIDE_BY_2/4/8)
 *     whatever the desktop's PLL is doing; the driver's own reset-path note
 *     calls a divided clock on a desktop PLL a likely trip under a CRT's 30 kHz
 *     floor, and .124 drives a 1998 CRT. So AA is refused unless the master's
 *     vidProcCfg says 2x mode - or the operator passes --force-desktop-pll.
 *     A driver whose vidProcCfg the tool cannot read counts as NOT 2x.
 *
 * Numbers are decimal or 0x-hex; anything else (a sign, a stray letter, an
 * empty field, more than 32 bits) is refused before anything is sent - the
 * same lesson as vcr_pace_parse_ms: strtoul reads "-1" as 0xffffffff.
 */
#ifndef VCR_SLIAA_H
#define VCR_SLIAA_H

#include "../include/vcr_types.h"
#include "../include/vcr_hwcext.h"
#include "../include/vcr_sli.h"     /* vcr_sli_combo_ok: the kernel's shape rule, inline */

#define VCR_SLIAA_AT_BOX_FLAG   "--i-am-at-the-box"
#define VCR_SLIAA_FORCE_PLL     "--force-desktop-pll"
/* what the tool knows of the master's vidProcCfg 2x mode (vcr_sliaa_pll_gate) */
#define VCR_SLIAA_2X_UNKNOWN    (-1)
/* optional fields when omitted: the band height of the goldens (cfg 5 on .124
 * ran 8-line bands) and 16 bpp; the memory fields 0, as Glide sends a base */
#define VCR_SLIAA_DEFAULT_NLINES 8u
#define VCR_SLIAA_DEFAULT_BPP    16u

typedef struct vcr_sliaa_cmd {
    int off;                    /* `sliaa off`: Glide's disable */
    int at_box;                 /* --i-am-at-the-box was given */
    int force_pll;              /* --force-desktop-pll was given */
    vcr_u32 n, sli, aa, high, analog;
    vcr_u32 nlines, bpp, tile, col, dlo, dhi;
} vcr_sliaa_cmd;

static int vcr_sliaa_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* strict unsigned: decimal, or 0x followed by 1..8 hex digits; 1 = ok */
static int vcr_sliaa_u32(const char *s, vcr_u32 *out)
{
    vcr_u32 v = 0, d;
    int digits = 0;
    if (!s || !*s)
        return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (s += 2; *s; s++, digits++) {
            if (*s >= '0' && *s <= '9')
                d = (vcr_u32)(*s - '0');
            else if (*s >= 'a' && *s <= 'f')
                d = (vcr_u32)(*s - 'a' + 10);
            else if (*s >= 'A' && *s <= 'F')
                d = (vcr_u32)(*s - 'A' + 10);
            else
                return 0;
            if (digits >= 8)
                return 0;
            v = (v << 4) | d;
        }
    } else {
        for (; *s; s++, digits++) {
            if (*s < '0' || *s > '9')
                return 0;
            d = (vcr_u32)(*s - '0');
            if (v > (0xffffffffu - d) / 10u)
                return 0;
            v = v * 10u + d;
        }
    }
    if (!digits)
        return 0;
    *out = v;
    return 1;
}

/* The arguments AFTER "sliaa". NULL = parsed; else why not (nothing sent).
 * The two flags may appear anywhere; any other "--" word is refused. */
static const char *vcr_sliaa_parse(int argc, char **argv, vcr_sliaa_cmd *c)
{
    vcr_u32 *const f[11] = { &c->n, &c->sli, &c->aa, &c->high, &c->analog,
                             &c->nlines, &c->bpp, &c->tile, &c->col, &c->dlo, &c->dhi };
    int i, k = 0;
    char *const *a = argv;
    c->off = c->at_box = c->force_pll = 0;
    c->n = c->sli = c->aa = c->high = c->analog = 0;
    c->nlines = VCR_SLIAA_DEFAULT_NLINES;
    c->bpp = VCR_SLIAA_DEFAULT_BPP;
    c->tile = c->col = c->dlo = c->dhi = 0;
    for (i = 0; i < argc; i++) {
        if (a[i][0] == '-' && a[i][1] == '-') {
            if (vcr_sliaa_streq(a[i], VCR_SLIAA_AT_BOX_FLAG))
                c->at_box = 1;
            else if (vcr_sliaa_streq(a[i], VCR_SLIAA_FORCE_PLL))
                c->force_pll = 1;
            else
                return "unknown option (only --i-am-at-the-box and --force-desktop-pll)";
            continue;
        }
        if (k == 0 && vcr_sliaa_streq(a[i], "off")) {
            c->off = 1;
            k = -1;             /* nothing may follow `off` but the flag */
            continue;
        }
        if (k < 0)
            return "`sliaa off` takes no values";
        if (k >= 11)
            return "too many values: n sli aa high analog [nlines bpp tileMark col depthlo depthhi]";
        if (!vcr_sliaa_u32(a[i], f[k]))
            return "a value is not a decimal or 0x-hex 32-bit number";
        k++;
    }
    if (c->off)
        return 0;
    if (k < 5)
        return "usage: sliaa <n> <sli> <aa> <high> <analog> [nlines bpp tileMark col depthlo "
               "depthhi] --i-am-at-the-box [--force-desktop-pll] | sliaa off";
    if (c->n != 1 && c->n != 2 && c->n != 4)
        return "n must be 1, 2 or 4";
    if (c->sli > 1 || c->aa > 1 || c->analog > 1)
        return "sli, aa and analog are 0 or 1";
    if (c->high > 2)
        return "high is 0 (2-sample), 1 (4-sample) or 2 (8-sample)";
    return 0;
}

/* NULL = may be sent; else why it must not be. Called before any escape. */
static const char *vcr_sliaa_gate(const vcr_sliaa_cmd *c)
{
    if (c->off)
        return 0;               /* the way back needs no flag */
    if (!c->at_box)
        return c->aa ? "an AA request can freeze the box hard (.124, 2026-09-26): refused - run "
                       "it only with a person at the box, and say so with --i-am-at-the-box"
                     : "an SLI enable reprograms the scan-out clocks (slave video PLLs, the 6000's "
                       "external clock): refused - run it only with a person at the box, and say "
                       "so with --i-am-at-the-box";
    if (!vcr_sli_combo_ok(c->n, c->sli, c->aa, c->high, c->analog))
        return "no video-mux branch for this chip/SLI/AA shape (the kernel's own rule, "
               "vcr_sli_combo_ok): refused - a vendor kernel would program it anyway, and "
               "AmigaMerlin's froze on cfg 1's {4,0,1,0,1}";
    return 0;
}

/* The AA-on-a-desktop-PLL gate, once the tool has asked the driver for the
 * master's vidProcCfg: vid2x = 1 (2x mode), 0 (not), or VCR_SLIAA_2X_UNKNOWN
 * (could not read it - not our driver). NULL = may be sent. */
static const char *vcr_sliaa_pll_gate(const vcr_sliaa_cmd *c, int vid2x)
{
    if (c->off || !c->aa || vid2x == 1 || c->force_pll)
        return 0;
    return vid2x == 0
        ? "AA divides the video clock by 2/4/8 and the desktop is not in 2x mode: that can take "
          "a CRT under its sync floor - refused; set a 2x desktop mode first, or pass "
          "--force-desktop-pll"
        : "AA divides the video clock by 2/4/8 and this driver's vidProcCfg cannot be read (is "
          "the desktop in 2x mode?): refused; pass --force-desktop-pll to send it anyway";
}

/* Glide's request (minihwc.c): chips, the flags, band height, swap algorithm
 * 1, totalMemory = the chip's memory in whole MB (Glide keeps h3Mem =
 * fbRam >> 20 and sends h3Mem * 1 MB), tileMark = tileCmpMark, the secondary
 * buffers, bpp. `off` is Glide's disable: dwChips and nothing else. */
static void vcr_sliaa_fill(const vcr_sliaa_cmd *c, vcr_u32 board_chips, vcr_u32 fb_bytes,
                           vcr_sli_aa_req *r)
{
    vcr_u32 i, *w = (vcr_u32 *)r;
    for (i = 0; i < sizeof *r / sizeof *w; i++)
        w[i] = 0;
    if (c->off) {
        r->ChipInfo.dwChips = board_chips ? board_chips : 1;
        return;
    }
    r->ChipInfo.dwChips = c->n;
    r->ChipInfo.dwsliEn = c->sli;
    r->ChipInfo.dwaaEn = c->aa;
    r->ChipInfo.dwaaSampleHigh = c->high;
    r->ChipInfo.dwsliAaAnalog = c->analog;
    r->ChipInfo.dwsli_nlines = c->nlines;
    r->ChipInfo.dwCfgSwapAlgorithm = 1;
    r->MemInfo.dwTotalMemory = (fb_bytes >> 20) << 20;
    r->MemInfo.dwTileMark = c->tile;
    r->MemInfo.dwTileCmpMark = c->tile;
    r->MemInfo.dwaaSecondaryColorBufBegin = c->col;
    r->MemInfo.dwaaSecondaryDepthBufBegin = c->dlo;
    r->MemInfo.dwaaSecondaryDepthBufEnd = c->dhi;
    r->MemInfo.dwBpp = c->bpp;
}

#endif /* VCR_SLIAA_H */
