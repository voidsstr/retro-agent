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

int vcr_hwcaps_set_monitor(struct vcr_hwcaps *hw, const vcr_edid_info *e)
{
    hw->mon_hmin_khz = hw->mon_hmax_khz = hw->mon_vmin_hz = hw->mon_vmax_hz = 0;
    hw->mon_max_pixclk_khz = 0;
    if (!e || !e->valid || !e->has_range)
        return 0;
    hw->mon_hmin_khz = e->hmin_khz;
    hw->mon_hmax_khz = e->hmax_khz;
    hw->mon_vmin_hz = e->vmin_hz;
    hw->mon_vmax_hz = e->vmax_hz;
    hw->mon_max_pixclk_khz = e->max_pixclk_khz;
    return 1;
}
