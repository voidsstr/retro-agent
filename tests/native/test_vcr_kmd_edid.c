/* test_vcr_kmd_edid.c
 *
 * The monitor's EDID and the mode filter it drives (voodoo-cleanroom/vcr-kmd/
 * common/vcr_edid.c, vcr_modes.c vcr_mode_check). Without it our driver
 * listed every timing to 85 Hz whatever was plugged in: the ICD asks for the
 * highest refresh listed, so a game ran 1600x1200 at 75 Hz (93.75 kHz) where
 * the vendor ran 60 - measured 4-5 % slower at fill-bound resolutions on the
 * V5 6000 (2026-09-26) - and a monitor whose range stops short of a listed
 * rate is driven out of range. Fixtures: the three monitors .124's registry
 * holds EDIDs for (serials blanked).
 *
 * And the fallback (vcr_mon_select / vcr_mon_boot, miniport/vcrmp_ddc.c
 * VcrMonitorInit): the list used to be UNFILTERED whenever the EDID could not
 * be read - the monitor off at boot, a KVM, a bad pair or checksum, Diag\Ddc
 * = 0 - which put 1600x1200@85 (106 kHz) and 1920x1440@75 (297 MHz) within
 * reach of .124's 96 kHz / 260 MHz Sony CPD-G200. The first fix (2026-09-26)
 * fell back to "the range of whichever monitor answered last", which hands
 * one tube another's limits: a range-less EDID from monitor B got monitor
 * A's range, and a boot with no EDID got the LAST monitor's range whatever
 * is plugged in. Now: the EDID's own range; else, for a range-less EDID, the
 * persisted range only when it is the same monitor's; else, with no EDID, the
 * ENVELOPE (the intersection of every monitor the box has read) BOUNDED BY
 * the conservative default - the second cut used the bare envelope, and a
 * tube with no DDC, or behind a KVM, is the no-EDID case itself and was never
 * narrowed into it; else the default (H 30-48 kHz, V 50-75 Hz, 80 MHz - the
 * first default's 70 kHz / 85 Hz was a mid-range tube's ceiling, not what
 * "any CRT of the era" takes). Diag\MonTrustEnvelope = 1 lifts the bound
 * (vcr_mon_trust_envelope); Diag\MonReset = 1 forgets the envelope. Which of
 * them decided is reported to the host as vcr_info.mon_src.
 */
#include <stddef.h>
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_edid.c"
#include "../../voodoo-cleanroom/vcr-kmd/common/vcr_modes.c"
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_ioctl.h"

/* Dell E2414H (DEL4090): the EDID XP stored under Enum\\DISPLAY on .124, serial
 * numbers blanked and the checksum recomputed */
static const vcr_u8 k_dell_e2414h[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x10, 0xac, 0x90, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x17, 0x18, 0x01, 0x03, 0x0e, 0x35, 0x1e, 0x78, 0xee, 0x0e, 0xf5, 0xa5, 0x55, 0x50, 0x9e, 0x26,
    0x10, 0x50, 0x54, 0xa5, 0x4b, 0x00, 0x71, 0x4f, 0x81, 0x80, 0xa9, 0xc0, 0xd1, 0xc0, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
    0x45, 0x00, 0x13, 0x2b, 0x21, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xff, 0x00, 0x30, 0x0a, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x44,
    0x45, 0x4c, 0x4c, 0x20, 0x45, 0x32, 0x34, 0x31, 0x34, 0x48, 0x0a, 0x20, 0x00, 0x00, 0x00, 0xfd,
    0x00, 0x38, 0x4c, 0x1e, 0x53, 0x11, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0xb3,
};

/* Gateway VX1120 (GWY0460): the EDID XP stored under Enum\\DISPLAY on .124, serial
 * numbers blanked and the checksum recomputed */
static const vcr_u8 k_gateway_vx1120[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x1e, 0xf9, 0x60, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x03, 0x0b, 0x01, 0x01, 0x0e, 0x28, 0x1e, 0x78, 0xe9, 0x04, 0x82, 0xa0, 0x57, 0x4a, 0x9b, 0x26,
    0x12, 0x48, 0x4f, 0xff, 0xff, 0x80, 0xe1, 0x4f, 0xa9, 0x4f, 0xc9, 0x4f, 0xc1, 0x4f, 0xa9, 0x59,
    0x81, 0x99, 0x61, 0x59, 0x45, 0x59, 0x04, 0x74, 0x80, 0xd0, 0x72, 0xa0, 0x3c, 0x50, 0x90, 0xe0,
    0x13, 0x00, 0x89, 0x27, 0x11, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0xa0, 0x1e,
    0x79, 0x24, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x47,
    0x61, 0x74, 0x65, 0x77, 0x61, 0x79, 0x56, 0x58, 0x31, 0x31, 0x32, 0x30, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x30, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x60,
};

/* Sony CPD-G200 (SNY1270): the EDID XP stored under Enum\\DISPLAY on .124, serial
 * numbers blanked and the checksum recomputed */
static const vcr_u8 k_sony_cpd_g200[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x4d, 0xd9, 0x70, 0x12, 0x00, 0x00, 0x00, 0x00,
    0x33, 0x09, 0x01, 0x02, 0x0e, 0x21, 0x18, 0x96, 0xeb, 0x0c, 0xc9, 0xa0, 0x57, 0x47, 0x9b, 0x27,
    0x12, 0x48, 0x4c, 0xff, 0xff, 0x80, 0x31, 0x59, 0x45, 0x59, 0x61, 0x59, 0x71, 0x59, 0x81, 0x99,
    0x81, 0x4f, 0xa9, 0x4f, 0x01, 0x01, 0xea, 0x24, 0x00, 0x60, 0x41, 0x00, 0x28, 0x30, 0x30, 0x60,
    0x13, 0x00, 0x38, 0xea, 0x10, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x30, 0x78, 0x1e,
    0x60, 0x1a, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x43,
    0x50, 0x44, 0x2d, 0x47, 0x32, 0x30, 0x30, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x30, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0xd1,
};

static const vcr_hwcaps k_v5 = { VCR_DEV_VSA100, 350000, 262000, 261, 32u << 20, 0x118000, 0,
                                 0, 0, 0, 0, 0 };

static int offered(const vcr_hwcaps *h, unsigned w, unsigned hh, unsigned hz, unsigned bpp)
{
    vcr_mode m[400];
    vcr_u32 n = vcr_modes_build(h, m, 400), i;
    for (i = 0; i < n; i++) {
        const vcr_timing *t = &vcr_timings[m[i].timing];
        if (t->w == w && t->h == hh && t->refresh == hz && m[i].bpp == bpp)
            return 1;
    }
    return 0;
}

TEST(the_three_monitors_parse) {
    vcr_edid_info d, g, s;
    CHECK(vcr_edid_parse(k_dell_e2414h, 128, &d), "Dell");
    CHECK(vcr_edid_parse(k_gateway_vx1120, 128, &g), "Gateway");
    CHECK(vcr_edid_parse(k_sony_cpd_g200, 128, &s), "Sony");
    CHECK(!strcmp(d.pnpid, "DEL") && d.product == 0x4090, "DEL4090");
    CHECK(!strcmp(g.pnpid, "GWY") && g.product == 0x0460, "GWY0460");
    CHECK(!strcmp(s.pnpid, "SNY") && s.product == 0x1270, "SNY1270");
    CHECK(!strcmp(d.name, "DELL E2414H"), "Dell name");
    CHECK(!strcmp(g.name, "GatewayVX1120"), "Gateway name");
    CHECK(!strcmp(s.name, "CPD-G200"), "Sony name");
    /* range limits, as the 0xFD descriptors state them */
    CHECK(d.has_range && s.has_range && g.has_range, "every one declares ranges");
    CHECK_EQ_U(d.vmin_hz, 56); CHECK_EQ_U(d.vmax_hz, 76);
    CHECK_EQ_U(d.hmin_khz, 30); CHECK_EQ_U(d.hmax_khz, 83);
    CHECK_EQ_U(d.max_pixclk_khz, 170000);
    CHECK_EQ_U(s.vmin_hz, 48); CHECK_EQ_U(s.vmax_hz, 120);
    CHECK_EQ_U(s.hmin_khz, 30); CHECK_EQ_U(s.hmax_khz, 96);
    CHECK_EQ_U(s.max_pixclk_khz, 260000);
    CHECK_EQ_U(g.vmin_hz, 50); CHECK_EQ_U(g.vmax_hz, 160);
    CHECK_EQ_U(g.hmin_khz, 30); CHECK_EQ_U(g.hmax_khz, 121);
    CHECK_EQ_U(g.max_pixclk_khz, 360000);
    /* preferred timings: the Dell panel's 1080p, the Sony's 1024x768@85 */
    CHECK_EQ_U(d.pref_w, 1920); CHECK_EQ_U(d.pref_h, 1080);
    CHECK_EQ_U(d.pref_pixclk_khz, 148500);
    CHECK_EQ_U(s.pref_w, 1024); CHECK_EQ_U(s.pref_h, 768);
    CHECK_EQ_U(s.pref_pixclk_khz, 94500);
    CHECK(s.pref_refresh_mhz > 84500 && s.pref_refresh_mhz < 85500, "Sony prefers 85 Hz");
    CHECK(!d.digital && !s.digital && !g.digital, "all three on the analog input");
}

/* the EDID's bytes, with the checksum made right again after an edit */
static void fix_checksum(vcr_u8 *e)
{
    unsigned i, sum = 0;
    for (i = 0; i < 127; i++)
        sum += e[i];
    e[127] = (vcr_u8)(0x100 - (sum & 0xff));
}

static void apply(vcr_hwcaps *h, const vcr_mon_range *r)
{
    *h = k_v5;
    vcr_hwcaps_set_range(h, r);
}

static int refused_by_monitor(const vcr_hwcaps *h, unsigned w, unsigned hh, unsigned hz)
{
    int i = vcr_timing_find(w, hh, hz);
    return i >= 0 && vcr_mode_check(h, &vcr_timings[i], 16) == VCR_MODE_E_MONITOR;
}

/* is w x h @ hz offered under range r (on the V5, 16 bpp)? */
static int offered_in(const vcr_mon_range *r, unsigned w, unsigned hh, unsigned hz)
{
    vcr_hwcaps h;
    apply(&h, r);
    return offered(&h, w, hh, hz, 16);
}

/* A monitor the fleet does not have: the Sony's EDID with another identity
 * and another range descriptor (its 0xFD sits at byte 72), checksum fixed. */
static void make_monitor(vcr_u8 *out, vcr_u16 pnp, vcr_u16 product, unsigned vmin, unsigned vmax,
                         unsigned hmin, unsigned hmax, unsigned pix_10mhz)
{
    memcpy(out, k_sony_cpd_g200, 128);
    out[8] = (vcr_u8)(pnp >> 8);
    out[9] = (vcr_u8)pnp;
    out[10] = (vcr_u8)product;
    out[11] = (vcr_u8)(product >> 8);
    out[72 + 5] = (vcr_u8)vmin;
    out[72 + 6] = (vcr_u8)vmax;
    out[72 + 7] = (vcr_u8)hmin;
    out[72 + 8] = (vcr_u8)hmax;
    out[72 + 9] = (vcr_u8)pix_10mhz;
    fix_checksum(out);
}

/* the same monitor's EDID with its range descriptor made a dummy (0x10) - as
 * an EDID before 1.3 commonly is */
static void without_ranges(vcr_u8 *out, const vcr_u8 *in, unsigned fd_at)
{
    memcpy(out, in, 128);
    out[fd_at + 3] = 0x10;
    fix_checksum(out);
}

static const vcr_mon_range k_sony_r = { 30, 96, 48, 120, 260000 };
static const vcr_mon_range k_dell_r = { 30, 83, 56, 76, 170000 };
/* the Sony and the Dell both accept exactly this */
static const vcr_mon_range k_sony_dell_r = { 30, 83, 56, 76, 170000 };

static int same_range(const vcr_mon_range *a, const vcr_mon_range *b)
{
    return a->hmin_khz == b->hmin_khz && a->hmax_khz == b->hmax_khz &&
           a->vmin_hz == b->vmin_hz && a->vmax_hz == b->vmax_hz &&
           a->max_pixclk_khz == b->max_pixclk_khz;
}

TEST(a_broken_edid_gives_no_limits_of_its_own) {
    vcr_u8 bad[128];
    vcr_edid_info e;
    vcr_hwcaps h = k_v5;
    memcpy(bad, k_sony_cpd_g200, 128);
    bad[100] ^= 1;                          /* checksum no longer 0 */
    CHECK(!vcr_edid_parse(bad, 128, &e) && !e.valid, "checksum failure refused");
    CHECK(!vcr_hwcaps_set_monitor(&h, &e), "no limits from it");
    CHECK_EQ_U(h.mon_hmax_khz, 0);
    CHECK(!vcr_edid_parse(k_sony_cpd_g200, 64, &e), "short buffer refused");
    CHECK_EQ_U(vcr_mon_id(&e), 0);
    CHECK_EQ_U(vcr_mon_id(NULL), 0);
    /* empty caps filter nothing - which is exactly why the miniport never
     * builds its list from them any more (vcr_mon_select below) */
    CHECK(offered(&h, 1600, 1200, 85, 16), "empty caps: the unfiltered table");
}

TEST(the_monitor_id_is_the_edid_s_own_identity) {
    /* MonId and VCR_EV_EDID's `a`: EDID bytes 8-9 (the three letters, big
     * endian, bit 15 reserved) | the product code << 16 */
    vcr_edid_info s, d, g;
    vcr_edid_parse(k_sony_cpd_g200, 128, &s);
    vcr_edid_parse(k_dell_e2414h, 128, &d);
    vcr_edid_parse(k_gateway_vx1120, 128, &g);
    CHECK_EQ_U(vcr_mon_id(&s), (0x4dd9u & 0x7fff) | (0x1270u << 16));
    CHECK_EQ_U(vcr_mon_id(&d), (0x10acu & 0x7fff) | (0x4090u << 16));
    CHECK_EQ_U(vcr_mon_id(&g), (0x1ef9u & 0x7fff) | (0x0460u << 16));
    CHECK(vcr_mon_id(&s) != vcr_mon_id(&d) && vcr_mon_id(&d) != vcr_mon_id(&g), "distinct");
    CHECK(!strcmp(vcr_mon_src_name(VCR_MON_SRC_EDID), "edid"), "edid");
    CHECK(!strcmp(vcr_mon_src_name(VCR_MON_SRC_SAME), "same-monitor persisted"), "same");
    CHECK(!strcmp(vcr_mon_src_name(VCR_MON_SRC_ENVELOPE), "envelope"), "envelope");
    CHECK(!strcmp(vcr_mon_src_name(VCR_MON_SRC_DEFAULT), "default"), "default");
    CHECK(!strcmp(vcr_mon_src_name(VCR_MON_SRC_NONE), "none"), "none");
}

TEST(a_good_edid_puts_its_own_range_in_force) {
    vcr_edid_info e, d;
    vcr_mon_range r;
    vcr_edid_parse(k_sony_cpd_g200, 128, &e);
    vcr_edid_parse(k_dell_e2414h, 128, &d);
    /* whatever is persisted, and whoever persisted it */
    CHECK_EQ_U(vcr_mon_select(&e, &k_dell_r, vcr_mon_id(&d), &r), VCR_MON_SRC_EDID);
    CHECK(same_range(&r, &k_sony_r), "the Sony's own");
    CHECK_EQ_U(vcr_mon_select(&e, &k_dell_r, vcr_mon_id(&e), &r), VCR_MON_SRC_EDID);
    CHECK(same_range(&r, &k_sony_r), "the Sony's own, not the envelope");
    CHECK_EQ_U(vcr_mon_select(&e, NULL, 0, &r), VCR_MON_SRC_EDID);
    CHECK_EQ_U(r.hmax_khz, 96);
}

TEST(no_edid_uses_the_envelope) {
    vcr_u8 bad[128];
    vcr_edid_info e, s;
    vcr_mon_range r, def;
    vcr_hwcaps h;
    vcr_edid_parse(k_sony_cpd_g200, 128, &s);
    vcr_mon_select(NULL, NULL, 0, &def);                /* the default, for comparison */
    /* the DDC read failed: the miniport hands over a zeroed, invalid info */
    memset(&e, 0, sizeof e);
    CHECK_EQ_U(vcr_mon_select(&e, &k_sony_r, vcr_mon_id(&s), &r), VCR_MON_SRC_ENVELOPE);
    CHECK_EQ_U(vcr_mon_select(NULL, &k_sony_r, vcr_mon_id(&s), &r), VCR_MON_SRC_ENVELOPE);
    /* who was seen last does not matter when nothing answered */
    CHECK_EQ_U(vcr_mon_select(NULL, &k_sony_r, 0, &r), VCR_MON_SRC_ENVELOPE);
    /* a bad checksum is the same case */
    memcpy(bad, k_sony_cpd_g200, 128);
    bad[100] ^= 1;
    vcr_edid_parse(bad, 128, &e);
    CHECK_EQ_U(vcr_mon_select(&e, &k_sony_r, vcr_mon_id(&s), &r), VCR_MON_SRC_ENVELOPE);
    /* fixed: the Sony's envelope n the default - which, the Sony being wider
     * everywhere, IS the default. Old (the bare envelope): the Sony's range
     * for whatever tube is there, DDC-less or behind a KVM. */
    CHECK(same_range(&r, &def), "the envelope, bounded by the default");
    CHECK(!same_range(&r, &k_sony_r), "old: the bare envelope");
    apply(&h, &r);
    CHECK(!offered(&h, 1600, 1200, 75, 16), "fixed: 1600x1200@75 (93.75 kHz) not offered");
    CHECK(offered_in(&k_sony_r, 1600, 1200, 75), "old: offered");
    CHECK(!offered(&h, 1600, 1200, 85, 16) && !offered(&h, 1920, 1440, 75, 16), "never");
    CHECK(refused_by_monitor(&h, 1600, 1200, 85), "refused as the MONITOR's limit");
    CHECK(offered(&h, 1024, 768, 60, 16), "1024x768@60 stays");
    /* Diag\MonTrustEnvelope = 1: the operator vouches for the tube - the
     * bare envelope, and the Sony's list */
    CHECK(vcr_mon_trust_envelope(VCR_MON_SRC_ENVELOPE, &k_sony_r, &r), "trusted");
    CHECK(same_range(&r, &k_sony_r), "the bare envelope");
    apply(&h, &r);
    CHECK(offered(&h, 1600, 1200, 75, 16), "93.75 kHz <= 96");
    CHECK(!offered(&h, 1600, 1200, 85, 16), "106 kHz > 96, trusted or not");
    CHECK(!offered(&h, 1920, 1440, 75, 16), "297 MHz > 260, trusted or not");
}

TEST(a_no_edid_boot_after_a_sony_only_history_gets_no_sony_rows) {
    /* The adversarial case of round 3: the box has only ever read the Sony
     * (96 kHz / 120 Hz / 260 MHz). The next boot's tube answers no DDC - a
     * 15-inch SVGA monitor, or anything behind a KVM - and the bare envelope
     * listed the Sony's rows for it. */
    vcr_edid_info sony, none;
    vcr_mon_range env = { 0, 0, 0, 0, 0 }, r;
    vcr_u32 id = 0, src;
    vcr_edid_parse(k_sony_cpd_g200, 128, &sony);
    memset(&none, 0, sizeof none);
    CHECK_EQ_U(vcr_mon_boot(&sony, 0, &env, &id, &r), VCR_MON_SRC_EDID);
    CHECK(same_range(&env, &k_sony_r), "the envelope is the Sony's alone");
    src = vcr_mon_boot(&none, 0, &env, &id, &r);
    CHECK_EQ_U(src, VCR_MON_SRC_ENVELOPE);
    CHECK(!offered_in(&r, 1280, 1024, 85), "fixed: no 1280x1024@85 (91 kHz)");
    CHECK(!offered_in(&r, 1024, 768, 100), "fixed: no 1024x768@100 (81 kHz, 100 Hz)");
    CHECK(!offered_in(&r, 1280, 1024, 60) && !offered_in(&r, 1024, 768, 70),
          "fixed: nothing past the default");
    CHECK(offered_in(&r, 1024, 768, 60) && offered_in(&r, 640, 480, 60), "the default's rows");
    CHECK(offered_in(&env, 1280, 1024, 85) && offered_in(&env, 1024, 768, 100),
          "old: the bare envelope offered both");
    /* the operator's override - the kernel applies it after vcr_mon_boot */
    CHECK(vcr_mon_trust_envelope(src, &env, &r), "MonTrustEnvelope=1 applies");
    CHECK(offered_in(&r, 1280, 1024, 85) && offered_in(&r, 1024, 768, 100),
          "trusted: the Sony's rows");
    /* and nowhere else: it never touches an EDID, same-monitor or default
     * decision, nor an envelope that is not usable */
    {
        vcr_mon_range keep = r, empty = { 60, 50, 48, 120, 260000 };
        CHECK(!vcr_mon_trust_envelope(VCR_MON_SRC_EDID, &env, &r), "EDID");
        CHECK(!vcr_mon_trust_envelope(VCR_MON_SRC_SAME, &env, &r), "SAME");
        CHECK(!vcr_mon_trust_envelope(VCR_MON_SRC_DEFAULT, &env, &r), "DEFAULT");
        CHECK(!vcr_mon_trust_envelope(VCR_MON_SRC_NONE, &env, &r), "NONE");
        CHECK(!vcr_mon_trust_envelope(VCR_MON_SRC_ENVELOPE, &empty, &r), "empty envelope");
        CHECK(!vcr_mon_trust_envelope(VCR_MON_SRC_ENVELOPE, NULL, &r), "no envelope");
        CHECK(same_range(&r, &keep), "left alone");
    }
}

TEST(no_edid_and_nothing_persisted_uses_the_safe_default) {
    vcr_edid_info e;
    vcr_mon_range none = { 0, 0, 0, 0, 0 }, r;
    vcr_hwcaps h;
    vcr_mode m[400];
    vcr_u32 n, i;
    static const unsigned yes[][3] = {
        { 640, 480, 60 }, { 640, 480, 72 }, { 640, 480, 75 },
        { 800, 600, 56 }, { 800, 600, 60 }, { 800, 600, 72 }, { 800, 600, 75 },
        { 1024, 768, 60 },                          /* 48.4 kHz: the +0.5 slack */
    };
    static const unsigned no[][3] = {
        { 640, 480, 85 }, { 800, 600, 85 },          /* 85 Hz > 75 */
        { 1024, 768, 70 }, { 1024, 768, 75 }, { 1024, 768, 85 },    /* 56.5+ kHz */
        { 1152, 864, 75 }, { 1280, 960, 60 },
        { 1280, 1024, 60 }, { 1280, 1024, 75 }, { 1280, 1024, 85 },
        { 1600, 1200, 60 }, { 1600, 1200, 85 }, { 1920, 1440, 75 },
    };
    memset(&e, 0, sizeof e);
    CHECK_EQ_U(vcr_mon_select(&e, &none, 0, &r), VCR_MON_SRC_DEFAULT);
    CHECK_EQ_U(vcr_mon_select(NULL, NULL, 0, &r), VCR_MON_SRC_DEFAULT);
    /* fixed: H 30-48 kHz, V 50-75 Hz, 80 MHz. Old: 30-70 / 50-85 / 135 -
     * 1024x768@85 (68.7 kHz), past what a 14" or 15" tube of 1995-98 takes */
    CHECK_EQ_U(r.hmin_khz, 30); CHECK_EQ_U(r.hmax_khz, 48);
    CHECK_EQ_U(r.vmin_hz, 50); CHECK_EQ_U(r.vmax_hz, 75);
    CHECK_EQ_U(r.max_pixclk_khz, 80000);
    CHECK(r.hmax_khz != 70 && r.vmax_hz != 85 && r.max_pixclk_khz != 135000, "old default");
    CHECK(vcr_mon_range_usable(&r), "the default passes its own test");
    apply(&h, &r);
    for (i = 0; i < sizeof yes / sizeof yes[0]; i++)
        CHECK(offered(&h, yes[i][0], yes[i][1], yes[i][2], 8) &&
              offered(&h, yes[i][0], yes[i][1], yes[i][2], 16) &&
              offered(&h, yes[i][0], yes[i][1], yes[i][2], 32), "the default admits it");
    for (i = 0; i < sizeof no / sizeof no[0]; i++) {
        CHECK(!offered(&h, no[i][0], no[i][1], no[i][2], 8) &&
              !offered(&h, no[i][0], no[i][1], no[i][2], 16) &&
              !offered(&h, no[i][0], no[i][1], no[i][2], 32), "the default refuses it");
        CHECK(refused_by_monitor(&h, no[i][0], no[i][1], no[i][2]), "as the monitor limit");
    }
    /* old: the first default offered 1024x768@85 to an unknown tube */
    {
        vcr_mon_range old = { 30, 70, 50, 85, 135000 };
        CHECK(offered_in(&old, 1024, 768, 85), "old: 1024x768@85, 68.7 kHz");
    }
    /* and nothing kept is outside it */
    n = vcr_modes_build(&h, m, 400);
    CHECK(n > 30, "a usable list");
    for (i = 0; i < n; i++) {
        const vcr_timing *t = &vcr_timings[m[i].timing];
        CHECK(vcr_timing_hfreq_hz(t) <= 48500 && vcr_timing_hfreq_hz(t) + 500 >= 30000, "H range");
        CHECK(vcr_timing_vfreq_mhz(t) <= 75500 && vcr_timing_vfreq_mhz(t) + 500 >= 50000, "V range");
        CHECK(t->pixclk_khz <= 80000, "dot clock");
        CHECK(t->w * t->h <= 1280u * 720u, "nothing wider than 1024x768 / 1280x720");
    }
}

TEST(an_edid_without_ranges_gets_its_own_persisted_range) {
    vcr_u8 e8[128];
    vcr_edid_info e;
    vcr_mon_range r;
    CHECK_EQ_U(k_sony_cpd_g200[72 + 3], 0xfd);   /* the Sony's range descriptor */
    without_ranges(e8, k_sony_cpd_g200, 72);
    CHECK(vcr_edid_parse(e8, 128, &e) && e.valid && !e.has_range, "valid, no ranges");
    CHECK(!strcmp(e.pnpid, "SNY") && e.product == 0x1270, "still the Sony");
    /* persisted by this same monitor */
    CHECK_EQ_U(vcr_mon_select(&e, &k_sony_r, vcr_mon_id(&e), &r), VCR_MON_SRC_SAME);
    CHECK(same_range(&r, &k_sony_r), "its own range");
    CHECK(offered_in(&r, 1600, 1200, 75) && !offered_in(&r, 1600, 1200, 85), "the Sony's list");
    /* nothing persisted, or an unusable envelope: the default */
    CHECK_EQ_U(vcr_mon_select(&e, NULL, vcr_mon_id(&e), &r), VCR_MON_SRC_DEFAULT);
    CHECK_EQ_U(r.hmax_khz, VCR_MON_DEF_HMAX_KHZ);
    {
        vcr_mon_range empty = { 60, 50, 48, 120, 260000 };
        CHECK_EQ_U(vcr_mon_select(&e, &empty, vcr_mon_id(&e), &r), VCR_MON_SRC_DEFAULT);
    }
}

TEST(an_edid_without_ranges_never_gets_another_monitors_range) {
    /* The Dell answers with a range-less EDID while the Sony's range is what
     * the box persisted. The first cut of the fallback gave the Dell the Sony's
     * H 96 kHz / V 120 Hz - a panel whose own limits are 83 / 76. */
    vcr_u8 e8[128];
    vcr_edid_info sony, dell;
    vcr_mon_range r;
    vcr_edid_parse(k_sony_cpd_g200, 128, &sony);
    CHECK_EQ_U(k_dell_e2414h[108 + 3], 0xfd);    /* the Dell's range descriptor */
    without_ranges(e8, k_dell_e2414h, 108);
    CHECK(vcr_edid_parse(e8, 128, &dell) && dell.valid && !dell.has_range, "Dell, no ranges");
    CHECK_EQ_U(vcr_mon_select(&dell, &k_sony_r, vcr_mon_id(&sony), &r), VCR_MON_SRC_DEFAULT);
    CHECK_EQ_U(r.hmax_khz, VCR_MON_DEF_HMAX_KHZ);        /* fixed: 48 */
    CHECK(r.hmax_khz != k_sony_r.hmax_khz, "old: the Sony's 96 kHz");
    CHECK_EQ_U(r.vmax_hz, VCR_MON_DEF_VMAX_HZ);          /* fixed: 75 */
    CHECK(r.vmax_hz != k_sony_r.vmax_hz, "old: the Sony's 120 Hz");
    /* what the Sony's range would have offered the Dell */
    CHECK(offered_in(&k_sony_r, 1600, 1200, 75), "old: 1600x1200@75, 93.7 kHz on an 83 kHz panel");
    CHECK(!offered_in(&r, 1600, 1200, 75), "fixed: not offered");
    CHECK(offered_in(&k_sony_r, 1280, 1024, 100) || offered_in(&k_sony_r, 1024, 768, 100),
          "old: 100 Hz rows on a 76 Hz panel");
    CHECK(!offered_in(&r, 1024, 768, 100), "fixed: not offered");
    /* an id of 0 matches nobody */
    CHECK_EQ_U(vcr_mon_select(&dell, &k_sony_r, 0, &r), VCR_MON_SRC_DEFAULT);
}

TEST(the_envelope_is_the_intersection_of_every_monitor_seen) {
    /* .124's registry holds EDIDs for the Sony, the Dell and the Gateway: all
     * three have been on this box. Boot by boot, through vcr_mon_boot. */
    vcr_edid_info sony, dell, gw, none;
    vcr_mon_range env = { 0, 0, 0, 0, 0 }, r;
    vcr_u32 id = 0;
    vcr_edid_parse(k_sony_cpd_g200, 128, &sony);
    vcr_edid_parse(k_dell_e2414h, 128, &dell);
    vcr_edid_parse(k_gateway_vx1120, 128, &gw);
    memset(&none, 0, sizeof none);

    CHECK_EQ_U(vcr_mon_boot(&dell, 0, &env, &id, &r), VCR_MON_SRC_EDID);
    CHECK(same_range(&env, &k_dell_r) && id == vcr_mon_id(&dell), "the first monitor: its own");
    CHECK_EQ_U(vcr_mon_boot(&sony, 0, &env, &id, &r), VCR_MON_SRC_EDID);
    CHECK(same_range(&r, &k_sony_r), "the Sony boots on its own range");
    CHECK_EQ_U(id, vcr_mon_id(&sony));
    /* fixed: max of the minima, min of the maxima and dot clocks */
    CHECK(same_range(&env, &k_sony_dell_r), "Sony n Dell");
    CHECK_EQ_U(env.hmax_khz, 83); CHECK_EQ_U(env.vmin_hz, 56); CHECK_EQ_U(env.vmax_hz, 76);
    CHECK_EQ_U(env.max_pixclk_khz, 170000);
    /* old: the last monitor's own range */
    CHECK(env.hmax_khz != 96 && env.vmax_hz != 120 && env.max_pixclk_khz != 260000,
          "old: the Sony's 96 kHz / 120 Hz / 260 MHz");
    /* the Gateway accepts more than both: nothing narrows, only the id moves */
    CHECK_EQ_U(vcr_mon_boot(&gw, 0, &env, &id, &r), VCR_MON_SRC_EDID);
    CHECK(same_range(&env, &k_sony_dell_r), "unchanged by a wider monitor");
    CHECK_EQ_U(id, vcr_mon_id(&gw));
    CHECK(!vcr_mon_envelope_add(&env, &k_sony_r), "adding a monitor again changes nothing");

    /* the boot with the monitor off: the envelope bounded by the default. The
     * envelope still NARROWS it - the Dell's 56 Hz floor - never widens it */
    CHECK_EQ_U(vcr_mon_boot(&none, 0, &env, &id, &r), VCR_MON_SRC_ENVELOPE);
    CHECK(same_range(&env, &k_sony_dell_r) && id == vcr_mon_id(&gw), "and not changed by it");
    CHECK_EQ_U(r.hmin_khz, 30); CHECK_EQ_U(r.hmax_khz, VCR_MON_DEF_HMAX_KHZ);
    CHECK_EQ_U(r.vmin_hz, 56);                          /* the Dell's, above the default's 50 */
    CHECK_EQ_U(r.vmax_hz, VCR_MON_DEF_VMAX_HZ);
    CHECK_EQ_U(r.max_pixclk_khz, VCR_MON_DEF_PIXCLK_KHZ);
    CHECK(offered_in(&r, 1024, 768, 60) && offered_in(&r, 800, 600, 56), "inside both");
    CHECK(!offered_in(&r, 1280, 1024, 75), "fixed: 1280x1024@75 (80 kHz) not for an unread tube");
    CHECK(!offered_in(&r, 1600, 1200, 60), "fixed: 1600x1200@60 (75 kHz) neither");
    CHECK(offered_in(&k_sony_dell_r, 1280, 1024, 75) && offered_in(&k_sony_dell_r, 1600, 1200, 60),
          "old: the bare envelope offered both");
    CHECK(!offered_in(&r, 1024, 768, 85), "1024x768@85: never");
    CHECK(!offered_in(&r, 1600, 1200, 70), "1600x1200@70: never");
    /* trusted (MonTrustEnvelope=1): the bare envelope - still every monitor's */
    CHECK(vcr_mon_trust_envelope(VCR_MON_SRC_ENVELOPE, &env, &r), "trusted");
    CHECK(same_range(&r, &k_sony_dell_r), "the bare envelope in force");
    CHECK(offered_in(&r, 1280, 1024, 75), "1280x1024@75: 80 kHz, 75 Hz, 135 MHz - all take it");
    CHECK(offered_in(&r, 1600, 1200, 60), "1600x1200@60: 75 kHz, 162 MHz");
    CHECK(!offered_in(&r, 1024, 768, 85), "1024x768@85: the Dell stops at 76 Hz");
    CHECK(!offered_in(&r, 1600, 1200, 70), "1600x1200@70: 87 kHz / 183 MHz, over the Dell");
    /* old: the last monitor's range (the Gateway's) offered both */
    {
        vcr_mon_range gw_r = { 30, 121, 50, 160, 360000 };
        CHECK(offered_in(&gw_r, 1024, 768, 85) && offered_in(&gw_r, 1600, 1200, 70),
              "old: the Gateway's range offered both to whatever was plugged in");
    }
    /* order does not matter */
    {
        vcr_mon_range e2 = { 0, 0, 0, 0, 0 };
        vcr_mon_envelope_add(&e2, &k_sony_r);
        vcr_mon_envelope_add(&e2, &k_dell_r);
        CHECK(same_range(&e2, &k_sony_dell_r), "Sony then Dell = Dell then Sony");
    }
}

TEST(a_zero_dot_clock_is_no_limit_not_the_smallest) {
    vcr_mon_range env = { 30, 96, 48, 120, 0 }, seen = { 30, 83, 56, 76, 170000 };
    vcr_mon_envelope_add(&env, &seen);
    CHECK_EQ_U(env.max_pixclk_khz, 170000);
    seen.max_pixclk_khz = 0;
    env.max_pixclk_khz = 260000;
    vcr_mon_envelope_add(&env, &seen);
    CHECK_EQ_U(env.max_pixclk_khz, 260000);
    /* an invalid monitor range narrows nothing */
    seen.hmax_khz = 0;
    CHECK(!vcr_mon_envelope_add(&env, &seen), "refused");
    CHECK_EQ_U(env.hmax_khz, 83);
}

TEST(monitors_that_share_no_range_leave_the_default) {
    /* a tube that stops at 50 kHz and a fixed-frequency one from 60: no
     * timing is safe on both, so a boot without an EDID gets the default -
     * where the first cut gave it whichever of them answered last */
    vcr_u8 lo8[128], hi8[128];
    vcr_edid_info lo, hi, sony, none;
    vcr_mon_range env = { 0, 0, 0, 0, 0 }, r;
    vcr_u32 id = 0;
    make_monitor(lo8, 0x1111, 0x0001, 50, 75, 30, 50, 8);     /* H 30-50, V 50-75, 80 MHz */
    make_monitor(hi8, 0x2222, 0x0002, 50, 90, 60, 100, 20);   /* H 60-100, V 50-90, 200 MHz */
    CHECK(vcr_edid_parse(lo8, 128, &lo) && lo.has_range, "lo parses");
    CHECK(vcr_edid_parse(hi8, 128, &hi) && hi.has_range, "hi parses");
    vcr_edid_parse(k_sony_cpd_g200, 128, &sony);
    memset(&none, 0, sizeof none);
    vcr_mon_boot(&lo, 0, &env, &id, &r);
    vcr_mon_boot(&hi, 0, &env, &id, &r);
    CHECK(env.hmin_khz == 60 && env.hmax_khz == 50, "empty: H 60 > 50, kept that way");
    CHECK(!vcr_mon_range_usable(&env), "empty is not usable");
    CHECK_EQ_U(vcr_mon_boot(&none, 0, &env, &id, &r), VCR_MON_SRC_DEFAULT);
    CHECK_EQ_U(r.hmax_khz, VCR_MON_DEF_HMAX_KHZ);
    CHECK(r.hmin_khz != 60 && r.hmax_khz != 100, "old: the last one's 60-100 kHz");
    /* the same monitor with a range-less EDID: still the default */
    CHECK_EQ_U(vcr_mon_select(&hi, &env, id, &r), VCR_MON_SRC_EDID);   /* has ranges */
    hi.has_range = 0;
    CHECK_EQ_U(vcr_mon_select(&hi, &env, id, &r), VCR_MON_SRC_DEFAULT);
    /* and it stays empty: a wider monitor does not bring a forgotten one back */
    vcr_mon_boot(&sony, 0, &env, &id, &r);
    CHECK(!vcr_mon_range_usable(&env), "still empty after the Sony");
    CHECK_EQ_U(vcr_mon_boot(&none, 0, &env, &id, &r), VCR_MON_SRC_DEFAULT);
}

TEST(an_envelope_without_vga_is_not_trusted) {
    /* not empty, but every tube on this box has shown the BIOS's 31.5 kHz text:
     * an envelope that cannot carry 640x480@60 is bad data, and a filter that
     * leaves no mode is a black screen, not protection */
    vcr_mon_range env = k_sony_r, noh = { 50, 121, 50, 160, 360000 }, r;
    vcr_hwcaps h;
    vcr_mon_envelope_add(&env, &noh);
    CHECK(env.hmin_khz == 50 && env.hmax_khz == 96, "H 50-96: valid, non-empty");
    CHECK(vcr_mon_range_valid(&env), "valid");
    CHECK(!vcr_mon_range_usable(&env), "but no 640x480@60 (31.5 kHz)");
    apply(&h, &env);
    CHECK(!offered(&h, 640, 480, 60, 8), "the list it would build has no VGA");
    CHECK_EQ_U(vcr_mon_select(NULL, &env, 0, &r), VCR_MON_SRC_DEFAULT);
    /* 60 Hz is the other half of it */
    {
        vcr_mon_range v = { 30, 96, 61, 120, 260000 };
        CHECK(!vcr_mon_range_usable(&v), "V from 61 Hz: no 640x480@60");
        v.vmin_hz = 60;
        CHECK(vcr_mon_range_usable(&v), "V from 60 Hz: 59.94 within the half-Hz slack");
    }
    /* the fleet's real envelopes all pass */
    CHECK(vcr_mon_range_usable(&k_sony_r) && vcr_mon_range_usable(&k_dell_r) &&
          vcr_mon_range_usable(&k_sony_dell_r), "Sony, Dell, Sony n Dell");
}

TEST(mon_reset_forgets_the_envelope) {
    /* Diag\MonReset = 1: the Dell has left .124 for good */
    vcr_edid_info sony, none;
    vcr_mon_range env = k_sony_dell_r, r;
    vcr_u32 id;
    vcr_edid_parse(k_sony_cpd_g200, 128, &sony);
    memset(&none, 0, sizeof none);
    id = vcr_mon_id(&sony);
    /* without the reset the Dell's 56 Hz floor still narrows a boot with no
     * EDID (and trusted, its 76 Hz ceiling limits it) */
    {
        vcr_mon_range e2 = env;
        vcr_u32 id2 = id, src;
        src = vcr_mon_boot(&none, 0, &e2, &id2, &r);
        CHECK_EQ_U(src, VCR_MON_SRC_ENVELOPE);
        CHECK_EQ_U(r.vmin_hz, 56);
        CHECK(vcr_mon_trust_envelope(src, &e2, &r), "trusted");
        CHECK_EQ_U(r.vmax_hz, 76);
    }
    /* the reset boot with the monitor off: nothing is known, so the default -
     * never the envelope it has just been told to forget */
    CHECK_EQ_U(vcr_mon_boot(&none, 1, &env, &id, &r), VCR_MON_SRC_DEFAULT);
    CHECK_EQ_U(r.hmax_khz, VCR_MON_DEF_HMAX_KHZ);
    CHECK_EQ_U(r.vmin_hz, VCR_MON_DEF_VMIN_HZ);
    CHECK(env.hmin_khz == 0 && env.hmax_khz == 0 && env.vmin_hz == 0 && env.vmax_hz == 0 &&
          env.max_pixclk_khz == 0 && id == 0, "cleared");
    /* the next boot the Sony answers: the envelope starts again from it alone */
    CHECK_EQ_U(vcr_mon_boot(&sony, 0, &env, &id, &r), VCR_MON_SRC_EDID);
    CHECK(same_range(&env, &k_sony_r), "the Sony's range, not Sony n Dell");
    CHECK_EQ_U(id, vcr_mon_id(&sony));
    /* a reset boot with the monitor on: cleared, then re-seeded at once */
    env = k_sony_dell_r;
    CHECK_EQ_U(vcr_mon_boot(&sony, 1, &env, &id, &r), VCR_MON_SRC_EDID);
    CHECK(same_range(&env, &k_sony_r), "re-seeded from the Sony alone");
    {
        vcr_u32 src = vcr_mon_boot(&none, 0, &env, &id, &r);
        CHECK_EQ_U(src, VCR_MON_SRC_ENVELOPE);
        CHECK_EQ_U(r.vmin_hz, VCR_MON_DEF_VMIN_HZ);    /* the Dell's 56 Hz is gone */
        CHECK(vcr_mon_trust_envelope(src, &env, &r), "trusted");
        CHECK_EQ_U(r.vmax_hz, 120);                     /* the Sony's own, not the Dell's 76 */
    }
}

TEST(an_absurd_persisted_range_is_not_trusted) {
    /* registry values an operator can edit: one that would switch the filter
     * off as surely as none must give way to the default */
    static const vcr_mon_range bad[] = {
        { 30, 0, 48, 120, 260000 },         /* no H maximum */
        { 30, 96, 48, 0, 260000 },          /* no V maximum */
        { 97, 96, 48, 120, 260000 },        /* H inverted */
        { 30, 96, 121, 120, 260000 },       /* V inverted */
        { 30, 0xffffffffu, 48, 120, 260000 },   /* past anything EDID can say */
        { 30, 96, 48, 0xffffffffu, 260000 },
        { 30, 96, 48, 120, 0xffffffffu },
    };
    vcr_u8 e8[128];
    vcr_edid_info same;
    vcr_mon_range r;
    unsigned i;
    without_ranges(e8, k_sony_cpd_g200, 72);
    vcr_edid_parse(e8, 128, &same);
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!vcr_mon_range_valid(&bad[i]), "refused");
        CHECK(!vcr_mon_range_usable(&bad[i]), "not usable");
        CHECK_EQ_U(vcr_mon_select(NULL, &bad[i], 0, &r), VCR_MON_SRC_DEFAULT);
        CHECK_EQ_U(r.hmax_khz, VCR_MON_DEF_HMAX_KHZ);
        CHECK_EQ_U(vcr_mon_select(&same, &bad[i], vcr_mon_id(&same), &r), VCR_MON_SRC_DEFAULT);
    }
}

TEST(the_sony_keeps_what_its_96_khz_allow) {
    vcr_edid_info e;
    vcr_hwcaps h = k_v5;
    vcr_mode all[400], mon[400];
    vcr_u32 na = vcr_modes_build(&h, all, 400), nm, i;
    vcr_edid_parse(k_sony_cpd_g200, 128, &e);
    CHECK(vcr_hwcaps_set_monitor(&h, &e), "limits applied");
    nm = vcr_modes_build(&h, mon, 400);
    CHECK(nm > 60 && nm < na, "some modes, fewer than unfiltered");
    CHECK(offered(&h, 1600, 1200, 75, 16), "93.75 kHz <= 96");
    CHECK(!offered(&h, 1600, 1200, 85, 16), "106 kHz > 96");
    CHECK(offered(&h, 1280, 1024, 85, 32), "91.1 kHz");
    CHECK(offered(&h, 640, 480, 60, 8), "31.5 kHz");
    CHECK(offered(&h, 1024, 768, 85, 16), "its preferred mode");
    /* nothing kept is outside the ranges */
    for (i = 0; i < nm; i++) {
        const vcr_timing *t = &vcr_timings[mon[i].timing];
        CHECK(vcr_timing_hfreq_hz(t) <= 96500 && vcr_timing_hfreq_hz(t) + 500 >= 30000, "H range");
        CHECK(vcr_timing_vfreq_mhz(t) <= 120500 && vcr_timing_vfreq_mhz(t) + 500 >= 48000, "V range");
        CHECK(t->pixclk_khz <= 260000, "dot clock");
    }
}

TEST(the_dell_panel_stops_at_76_hz_and_170_mhz) {
    vcr_edid_info e;
    vcr_hwcaps h = k_v5;
    vcr_edid_parse(k_dell_e2414h, 128, &e);
    vcr_hwcaps_set_monitor(&h, &e);
    CHECK(offered(&h, 1280, 1024, 75, 16), "80 kHz, 75 Hz");
    CHECK(!offered(&h, 1280, 1024, 85, 16), "91 kHz is above 83");
    CHECK(offered(&h, 1024, 768, 75, 16), "60 kHz");
    CHECK(!offered(&h, 1024, 768, 85, 16), "85 Hz is above 76");
    CHECK(offered(&h, 1600, 1200, 60, 16), "156.6 MHz <= 170");
    CHECK(!offered(&h, 1600, 1200, 70, 16), "182.7 MHz > 170");
}

TEST(a_75_hz_timing_at_75_03_is_not_refused_by_rounding) {
    /* the slack: EDID stores whole Hz and kHz */
    vcr_hwcaps h = k_v5;
    int i = vcr_timing_find(1024, 768, 75);
    CHECK(i >= 0, "1024x768@75 exists");
    h.mon_hmin_khz = 30; h.mon_hmax_khz = vcr_timing_hfreq_hz(&vcr_timings[i]) / 1000;
    h.mon_vmin_hz = 50;  h.mon_vmax_hz = 75;
    CHECK_EQ_I(vcr_mode_check(&h, &vcr_timings[i], 16), 0);
    h.mon_vmax_hz = 74;
    CHECK_EQ_I(vcr_mode_check(&h, &vcr_timings[i], 16), VCR_MODE_E_MONITOR);
}

TEST(vcr_info_reports_the_source_at_its_end) {
    /* mon_src is APPENDED: every older field keeps its offset, so a tool
     * built before it still reads the rest - and the miniport answers such a
     * tool with the struct as it was (IOCTL_VCR_INFO, vcrmp.c). */
    CHECK_EQ_U(offsetof(vcr_info, glide_chips), 228);
    CHECK_EQ_U(offsetof(vcr_info, edid_ok), 260);
    CHECK_EQ_U(offsetof(vcr_info, mon_name), 296);
    CHECK_EQ_U(offsetof(vcr_info, edid), 312);
    CHECK_EQ_U(offsetof(vcr_info, mon_src), 312 + 128);
    CHECK_EQ_U(sizeof(vcr_info), 444);          /* was 440, ending at edid[127] */
    CHECK_EQ_U(offsetof(vcr_info, mon_src) + sizeof(vcr_u32), sizeof(vcr_info));
    /* the values it carries are vcr_edid.h's, which the host reads by number */
    CHECK_EQ_U(VCR_MON_SRC_NONE, 0);
    CHECK_EQ_U(VCR_MON_SRC_EDID, 1);
    CHECK_EQ_U(VCR_MON_SRC_SAME, 2);
    CHECK_EQ_U(VCR_MON_SRC_ENVELOPE, 3);
    CHECK_EQ_U(VCR_MON_SRC_DEFAULT, 4);
}

MUNIT_MAIN("vcr-kmd EDID + monitor mode filter", {
    RUN(the_three_monitors_parse);
    RUN(a_broken_edid_gives_no_limits_of_its_own);
    RUN(the_monitor_id_is_the_edid_s_own_identity);
    RUN(a_good_edid_puts_its_own_range_in_force);
    RUN(no_edid_uses_the_envelope);
    RUN(a_no_edid_boot_after_a_sony_only_history_gets_no_sony_rows);
    RUN(no_edid_and_nothing_persisted_uses_the_safe_default);
    RUN(an_edid_without_ranges_gets_its_own_persisted_range);
    RUN(an_edid_without_ranges_never_gets_another_monitors_range);
    RUN(the_envelope_is_the_intersection_of_every_monitor_seen);
    RUN(a_zero_dot_clock_is_no_limit_not_the_smallest);
    RUN(monitors_that_share_no_range_leave_the_default);
    RUN(an_envelope_without_vga_is_not_trusted);
    RUN(mon_reset_forgets_the_envelope);
    RUN(an_absurd_persisted_range_is_not_trusted);
    RUN(the_sony_keeps_what_its_96_khz_allow);
    RUN(the_dell_panel_stops_at_76_hz_and_170_mhz);
    RUN(a_75_hz_timing_at_75_03_is_not_refused_by_rounding);
    RUN(vcr_info_reports_the_source_at_its_end);
})
