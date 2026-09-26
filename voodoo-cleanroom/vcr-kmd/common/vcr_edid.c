/*
 * vcr_edid.c - see include/vcr_edid.h. Layout from the VESA E-EDID standard
 * (base block, 18-byte descriptors at 54/72/90/108).
 */
#include "../include/vcr_edid.h"
#include "../include/vcr_modes.h"

static void copy_text(char *dst, const vcr_u8 *src, vcr_u32 n)
{
    vcr_u32 i, end = 0;
    for (i = 0; i < n && src[i] != 0x0a && src[i] != 0x00; i++) {
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? (char)src[i] : '?';
        if (src[i] != ' ')
            end = i + 1;
    }
    dst[end] = 0;
}

int vcr_edid_parse(const vcr_u8 *e, vcr_u32 len, vcr_edid_info *o)
{
    static const vcr_u8 hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    vcr_u32 i, sum = 0, id;
    vcr_u8 *p = (vcr_u8 *)o;

    for (i = 0; i < sizeof *o; i++)
        p[i] = 0;
    if (!e || len < VCR_EDID_BLOCK)
        return 0;
    for (i = 0; i < 8; i++)
        if (e[i] != hdr[i])
            return 0;
    for (i = 0; i < VCR_EDID_BLOCK; i++)
        sum += e[i];
    if (sum & 0xff)
        return 0;

    id = ((vcr_u32)e[8] << 8) | e[9];           /* three 5-bit letters, big endian */
    o->pnpid[0] = (char)('@' + ((id >> 10) & 0x1f));
    o->pnpid[1] = (char)('@' + ((id >> 5) & 0x1f));
    o->pnpid[2] = (char)('@' + (id & 0x1f));
    o->product = (vcr_u16)(e[10] | (e[11] << 8));
    o->version = e[18];
    o->revision = e[19];
    o->digital = (e[20] & 0x80) ? 1 : 0;
    o->width_cm = e[21];
    o->height_cm = e[22];
    o->extensions = e[126];

    for (i = 54; i + 18 <= 126; i += 18) {
        const vcr_u8 *d = e + i;
        vcr_u32 pix = d[0] | (d[1] << 8);
        if (pix) {                              /* a detailed timing */
            if (i == 54) {
                vcr_u32 ha = d[2] | ((d[4] & 0xf0) << 4), hb = d[3] | ((d[4] & 0x0f) << 8);
                vcr_u32 va = d[5] | ((d[7] & 0xf0) << 4), vb = d[6] | ((d[7] & 0x0f) << 8);
                o->pref_w = ha;
                o->pref_h = va;
                o->pref_pixclk_khz = pix * 10;
                if (ha + hb && va + vb) {
                    o->pref_hfreq_hz = o->pref_pixclk_khz * 1000u / (ha + hb);
                    o->pref_refresh_mhz = o->pref_hfreq_hz * 1000u / (va + vb);
                }
            }
            continue;
        }
        if (d[2] != 0)
            continue;
        if (d[3] == 0xfc) {                     /* monitor name */
            copy_text(o->name, d + 5, 13);
        } else if (d[3] == 0xfd) {              /* display range limits */
            /* EDID 1.4 byte 4: +255 offsets for the rates */
            vcr_u32 off = o->revision >= 4 ? d[4] : 0;
            o->vmin_hz = d[5] + ((off & 0x01) ? 255 : 0);
            o->vmax_hz = d[6] + ((off & 0x02) ? 255 : 0);
            o->hmin_khz = d[7] + ((off & 0x04) ? 255 : 0);
            o->hmax_khz = d[8] + ((off & 0x08) ? 255 : 0);
            o->max_pixclk_khz = d[9] * 10000u;
            o->has_range = o->vmax_hz && o->hmax_khz && o->vmin_hz <= o->vmax_hz &&
                           o->hmin_khz <= o->hmax_khz;
        }
    }
    o->valid = 1;
    return 1;
}

static int edid_range(const vcr_edid_info *e, vcr_mon_range *r)
{
    if (!e || !e->valid || !e->has_range)
        return 0;
    r->hmin_khz = e->hmin_khz;
    r->hmax_khz = e->hmax_khz;
    r->vmin_hz = e->vmin_hz;
    r->vmax_hz = e->vmax_hz;
    r->max_pixclk_khz = e->max_pixclk_khz;
    return 1;
}

int vcr_hwcaps_set_monitor(struct vcr_hwcaps *hw, const vcr_edid_info *e)
{
    vcr_mon_range r;
    if (!edid_range(e, &r)) {
        vcr_hwcaps_set_range(hw, 0);
        return 0;
    }
    vcr_hwcaps_set_range(hw, &r);
    return 1;
}

void vcr_hwcaps_set_range(struct vcr_hwcaps *hw, const vcr_mon_range *r)
{
    hw->mon_hmin_khz = r ? r->hmin_khz : 0;
    hw->mon_hmax_khz = r ? r->hmax_khz : 0;
    hw->mon_vmin_hz = r ? r->vmin_hz : 0;
    hw->mon_vmax_hz = r ? r->vmax_hz : 0;
    hw->mon_max_pixclk_khz = r ? r->max_pixclk_khz : 0;
}

/* EDID 1.4 encodes rates up to 255 + 255 and a dot clock up to 2550 MHz;
 * anything past that did not come from a monitor. */
int vcr_mon_range_valid(const vcr_mon_range *r)
{
    return r && r->hmax_khz && r->vmax_hz && r->hmin_khz <= r->hmax_khz &&
           r->vmin_hz <= r->vmax_hz && r->hmax_khz <= 510 && r->vmax_hz <= 510 &&
           r->max_pixclk_khz <= 2550000u;
}

int vcr_mon_range_usable(const vcr_mon_range *r)
{
    vcr_hwcaps h;
    int vga = vcr_timing_find(640, 480, 60);
    if (!vcr_mon_range_valid(r) || vga < 0)
        return 0;
    /* only the monitor half of the check may refuse it: the chip limits are
     * set out of the way, so this asks exactly what the list would */
    h.device_id = 0;
    h.max_pixclk_khz = 0xffffffffu;
    h.twox_above_khz = 0;
    h.twox_htotal_chars = 0;
    h.fb_bytes = 0xffffffffu;
    h.fb_reserved = 0;
    h.napalm_vpc_extra = 0;
    vcr_hwcaps_set_range(&h, r);
    return vcr_mode_check(&h, &vcr_timings[vga], 8) == 0;
}

vcr_u32 vcr_mon_id(const vcr_edid_info *e)
{
    if (!e || !e->valid)
        return 0;
    return ((vcr_u32)(e->pnpid[0] - '@') << 10) | ((vcr_u32)(e->pnpid[1] - '@') << 5) |
           (vcr_u32)(e->pnpid[2] - '@') | ((vcr_u32)e->product << 16);
}

const char *vcr_mon_src_name(vcr_u32 src)
{
    switch (src) {
    case VCR_MON_SRC_EDID:      return "edid";
    case VCR_MON_SRC_SAME:      return "same-monitor persisted";
    case VCR_MON_SRC_ENVELOPE:  return "envelope";
    case VCR_MON_SRC_DEFAULT:   return "default";
    default:                    return "none";
    }
}

static void mon_default(vcr_mon_range *r)
{
    r->hmin_khz = VCR_MON_DEF_HMIN_KHZ;
    r->hmax_khz = VCR_MON_DEF_HMAX_KHZ;
    r->vmin_hz = VCR_MON_DEF_VMIN_HZ;
    r->vmax_hz = VCR_MON_DEF_VMAX_HZ;
    r->max_pixclk_khz = VCR_MON_DEF_PIXCLK_KHZ;
}

vcr_u32 vcr_mon_select(const vcr_edid_info *e, const vcr_mon_range *env, vcr_u32 env_id,
                       vcr_mon_range *out)
{
    vcr_mon_range def;
    mon_default(&def);
    if (edid_range(e, out) && vcr_mon_range_valid(out))
        return VCR_MON_SRC_EDID;
    if (e && e->valid) {
        /* A monitor that answered but states no range. Only its OWN persisted
         * range may stand in: the first cut of this fallback (2026-09-26) took
         * whichever monitor answered last, and so would have driven this one
         * by a stranger's limits - a 96 kHz tube's range on a 60 kHz one. */
        if (env_id && env_id == vcr_mon_id(e) && vcr_mon_range_usable(env)) {
            *out = *env;
            return VCR_MON_SRC_SAME;
        }
    } else if (vcr_mon_range_usable(env)) {
        /* Nothing answered, so it may be any tube this box has had - or one
         * it never read: a tube without DDC, or behind a KVM that eats it, is
         * exactly this case, and its range was never narrowed into the
         * envelope. So the envelope may narrow the default (a 60 Hz panel
         * seen here keeps V at 60), never widen it: the second cut
         * (2026-09-26) used the bare envelope, which after a Sony-only
         * history listed 1280x1024@85 (91 kHz) for whatever tube was there.
         * A usable envelope meets the default in VGA, so the result is
         * usable too; the check below only keeps that true by construction. */
        *out = *env;
        vcr_mon_envelope_add(out, &def);
        if (vcr_mon_range_usable(out))
            return VCR_MON_SRC_ENVELOPE;
    }
    *out = def;
    return VCR_MON_SRC_DEFAULT;
}

int vcr_mon_trust_envelope(vcr_u32 src, const vcr_mon_range *env, vcr_mon_range *out)
{
    if (src != VCR_MON_SRC_ENVELOPE || !vcr_mon_range_usable(env))
        return 0;
    *out = *env;
    return 1;
}

int vcr_mon_envelope_add(vcr_mon_range *env, const vcr_mon_range *seen)
{
    vcr_mon_range n;
    if (!vcr_mon_range_valid(seen))
        return 0;
    if (!env->hmax_khz) {
        n = *seen;              /* the first monitor this box has seen */
    } else {
        n.hmin_khz = env->hmin_khz > seen->hmin_khz ? env->hmin_khz : seen->hmin_khz;
        n.hmax_khz = env->hmax_khz < seen->hmax_khz ? env->hmax_khz : seen->hmax_khz;
        n.vmin_hz = env->vmin_hz > seen->vmin_hz ? env->vmin_hz : seen->vmin_hz;
        n.vmax_hz = env->vmax_hz < seen->vmax_hz ? env->vmax_hz : seen->vmax_hz;
        n.max_pixclk_khz = !env->max_pixclk_khz ? seen->max_pixclk_khz
                         : !seen->max_pixclk_khz ? env->max_pixclk_khz
                         : env->max_pixclk_khz < seen->max_pixclk_khz ? env->max_pixclk_khz
                                                                      : seen->max_pixclk_khz;
    }
    if (n.hmin_khz == env->hmin_khz && n.hmax_khz == env->hmax_khz &&
        n.vmin_hz == env->vmin_hz && n.vmax_hz == env->vmax_hz &&
        n.max_pixclk_khz == env->max_pixclk_khz)
        return 0;
    *env = n;
    return 1;
}

vcr_u32 vcr_mon_boot(const vcr_edid_info *e, vcr_u32 reset, vcr_mon_range *env,
                     vcr_u32 *env_id, vcr_mon_range *out)
{
    vcr_u32 src;
    if (reset) {
        /* before the choice, so the boot that resets never uses what it forgot */
        env->hmin_khz = env->hmax_khz = env->vmin_hz = env->vmax_hz = 0;
        env->max_pixclk_khz = 0;
        *env_id = 0;
    }
    src = vcr_mon_select(e, env, *env_id, out);
    if (src == VCR_MON_SRC_EDID) {
        vcr_mon_envelope_add(env, out);
        *env_id = vcr_mon_id(e);
    }
    return src;
}
