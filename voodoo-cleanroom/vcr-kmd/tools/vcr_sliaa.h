/*
 * vcr_sliaa.h - `vcrctl sliaa`: the argument parser, the safety gate and the
 * request builder, as pure C (no Win32, no CRT), so the host test compiles
 * the code the tool runs (tests/native/test_vcr_kmd_sli.c).
 *
 *   vcrctl sliaa <n> <sli> <aa> <high> <analog> [nlines bpp tileMark col depthlo depthhi]
 *   vcrctl sliaa off
 *   ... --i-am-at-the-box      REQUIRED for any request with aa = 1
 *
 * It sends HWCEXT_SLI_AA_REQUEST through ExtEscape exactly as Glide does
 * (minihwc.c, the HWC_MINIVDD_HACK block): the same payload, the same escape,
 * and - on our driver - the same kernel path, policy and all. So it is the
 * kernel-only probe of an SLI/AA configuration: no grSstWinOpen, no LFB, no
 * DAC - just what the kernel programs, which config-space reads (`vcrctl
 * pci`) and Diag\SliAAState then show.
 *
 * WHY THE FLAG. Every AA configuration tried on the V5 6000 froze .124 hard
 * (2026-09-26), and a frozen box needs a person at its power switch. The
 * kernel refuses AA unless Diag\SliAA = 1; this refuses it unless the
 * operator says, on the command line, that a person is at the box - defence
 * in depth, so neither a stray registry value nor a copied command line alone
 * is enough. The gate runs before the tool opens the escape: a refused
 * request sends nothing.
 *
 * Numbers are decimal or 0x-hex; anything else (a sign, a stray letter, an
 * empty field, more than 32 bits) is refused before anything is sent - the
 * same lesson as vcr_pace_parse_ms: strtoul reads "-1" as 0xffffffff.
 */
#ifndef VCR_SLIAA_H
#define VCR_SLIAA_H

#include "../include/vcr_types.h"
#include "../include/vcr_hwcext.h"

#define VCR_SLIAA_AT_BOX_FLAG   "--i-am-at-the-box"
/* optional fields when omitted: the band height of the goldens (cfg 5 on .124
 * ran 8-line bands) and 16 bpp; the memory fields 0, as Glide sends a base */
#define VCR_SLIAA_DEFAULT_NLINES 8u
#define VCR_SLIAA_DEFAULT_BPP    16u

typedef struct vcr_sliaa_cmd {
    int off;                    /* `sliaa off`: Glide's disable */
    int at_box;                 /* --i-am-at-the-box was given */
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
 * The flag may appear anywhere; any other "--" word is refused. */
static const char *vcr_sliaa_parse(int argc, char **argv, vcr_sliaa_cmd *c)
{
    vcr_u32 *const f[11] = { &c->n, &c->sli, &c->aa, &c->high, &c->analog,
                             &c->nlines, &c->bpp, &c->tile, &c->col, &c->dlo, &c->dhi };
    int i, k = 0;
    char *const *a = argv;
    c->off = c->at_box = 0;
    c->n = c->sli = c->aa = c->high = c->analog = 0;
    c->nlines = VCR_SLIAA_DEFAULT_NLINES;
    c->bpp = VCR_SLIAA_DEFAULT_BPP;
    c->tile = c->col = c->dlo = c->dhi = 0;
    for (i = 0; i < argc; i++) {
        if (a[i][0] == '-' && a[i][1] == '-') {
            if (!vcr_sliaa_streq(a[i], VCR_SLIAA_AT_BOX_FLAG))
                return "unknown option (the only one is --i-am-at-the-box)";
            c->at_box = 1;
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
               "depthhi] [--i-am-at-the-box] | sliaa off";
    if (c->n != 1 && c->n != 2 && c->n != 4)
        return "n must be 1, 2 or 4";
    if (c->sli > 1 || c->aa > 1 || c->analog > 1)
        return "sli, aa and analog are 0 or 1";
    if (c->high > 2)
        return "high is 0 (2-sample), 1 (4-sample) or 2 (8-sample)";
    return 0;
}

/* NULL = may be sent; else why it must not be. Called before the escape. */
static const char *vcr_sliaa_gate(const vcr_sliaa_cmd *c)
{
    if (!c->off && c->aa && !c->at_box)
        return "an AA request can freeze the box hard (.124, 2026-09-26): refused - run it only "
               "with a person at the box, and say so with --i-am-at-the-box";
    return 0;
}

/* Glide's request (minihwc.c): chips, the flags, band height, swap algorithm
 * 1, totalMemory = the chip's memory, tileMark = tileCmpMark, the secondary
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
    r->MemInfo.dwTotalMemory = fb_bytes;
    r->MemInfo.dwTileMark = c->tile;
    r->MemInfo.dwTileCmpMark = c->tile;
    r->MemInfo.dwaaSecondaryColorBufBegin = c->col;
    r->MemInfo.dwaaSecondaryDepthBufBegin = c->dlo;
    r->MemInfo.dwaaSecondaryDepthBufEnd = c->dhi;
    r->MemInfo.dwBpp = c->bpp;
}

#endif /* VCR_SLIAA_H */
