/*
 * vcrmp_ddc.c - the monitor: its EDID over DDC, and the monitor child device.
 *
 * The VSA-100 / Voodoo 3 DDC pair is bits 18-22 of vidSerialParallelPort
 * (enable, clock out, data out, clock in, data in; the outputs are open
 * drain - 1 releases the line). videoprt's VideoPortDDCMonitorHelper drives
 * the I2C protocol through the four callbacks below. The vendor driver runs
 * with the DDC pair enabled and released (0xc37c0020 on .124); so do we.
 *
 * The EDID is read once at FindAdapter, because the mode list is built there
 * and filtered by the monitor's range limits (common/vcr_edid.c), and handed
 * to XP as the monitor child's descriptor. Every good EDID's range is also
 * narrowed into a persisted ENVELOPE (Diag\MonHminKhz ... MonMaxPixclkKhz:
 * the intersection of every monitor this box has read; MonId: the last one),
 * so a boot without an EDID - the monitor off at power-on, a KVM, a bad pair
 * or checksum, Diag\Ddc = 0 - is filtered by what EVERY tube seen here
 * accepts, bounded by the conservative default: the envelope only knows the
 * tubes whose EDID was read, and the one that cannot answer DDC is the
 * no-EDID case itself. Diag\MonTrustEnvelope = 1 lifts that bound for an
 * operator who knows the tube behind the KVM (a WARN every boot it is used).
 * An EDID that states no range gets the persisted one only when MonId says it
 * is the same monitor. Anything else gets the default (vcr_mon_select). No
 * EDID used to mean no filter, which put 1600x1200@85 / 106 kHz within reach
 * of .124's 96 kHz tube. Diag\MonReset = 1 forgets the envelope (for a tube
 * that has left the box for good; the driver sets it back to 0). Only
 * Diag\EdidFilter = 0 lists every mode, and it says so at WARN. Whatever
 * decided, x->mon_src records it and vcrctl info reports it (mon_src).
 */
#include "vcrmp.h"

static void spp_set(VCR_EXT *x, ULONG bit, UCHAR on)
{
    ULONG r = VcrRd(x, 0, VCR_R_VIDSERIALPARALLELPORT);
    VcrWr(x, 0, VCR_R_VIDSERIALPARALLELPORT, on ? (r | bit) : (r & ~bit));
    (void)VcrRd(x, 0, VCR_R_VIDSERIALPARALLELPORT);        /* flush the posted write */
}

static VOID NTAPI ddc_wr_clk(PVOID ext, UCHAR v)
{
    spp_set((VCR_EXT *)ext, VCR_SPP_DDC_DCK_OUT, v);
}

static VOID NTAPI ddc_wr_dat(PVOID ext, UCHAR v)
{
    spp_set((VCR_EXT *)ext, VCR_SPP_DDC_DDA_OUT, v);
}

static BOOLEAN NTAPI ddc_rd_clk(PVOID ext)
{
    return (VcrRd((VCR_EXT *)ext, 0, VCR_R_VIDSERIALPARALLELPORT) & VCR_SPP_DDC_DCK_IN) != 0;
}

static BOOLEAN NTAPI ddc_rd_dat(PVOID ext)
{
    return (VcrRd((VCR_EXT *)ext, 0, VCR_R_VIDSERIALPARALLELPORT) & VCR_SPP_DDC_DDA_IN) != 0;
}

static ULONG ddc_read(VCR_EXT *x, PUCHAR buf, ULONG len)
{
    DDC_CONTROL dc;
    ULONG before, ok;

    before = VcrRd(x, 0, VCR_R_VIDSERIALPARALLELPORT);
    VcrWr(x, 0, VCR_R_VIDSERIALPARALLELPORT,
          before | VCR_SPP_DDC_EN | VCR_SPP_DDC_DCK_OUT | VCR_SPP_DDC_DDA_OUT);
    VideoPortStallExecution(50);
    VideoPortZeroMemory(&dc, sizeof dc);
    VideoPortZeroMemory(buf, len);
    dc.Size = sizeof dc;
    dc.I2CCallbacks.WriteClockLine = ddc_wr_clk;
    dc.I2CCallbacks.WriteDataLine = ddc_wr_dat;
    dc.I2CCallbacks.ReadClockLine = ddc_rd_clk;
    dc.I2CCallbacks.ReadDataLine = ddc_rd_dat;
    dc.EdidSegment = 0;
    ok = VideoPortDDCMonitorHelper(x, &dc, buf, len) ? 1 : 0;
    VLOG(ok ? VCR_LV_INFO : VCR_LV_WARN, VCR_EV_DDC, ok, before,
         VcrRd(x, 0, VCR_R_VIDSERIALPARALLELPORT), len,
         ok ? "EDID read over DDC" : "no EDID over DDC (no DDC monitor, a KVM, or a dead pair)");
    return ok;
}

/* a range as VCR_EV_EDID's b and c carry it (d: the dot clock) */
#define RANGE_B(r) ((r).hmin_khz | ((r).hmax_khz << 16))
#define RANGE_C(r) ((r).vmin_hz | ((r).vmax_hz << 16))

static void mon_load(vcr_mon_range *r, vcr_u32 *id)
{
    r->hmin_khz = VcrDiagGet(L"MonHminKhz", 0);
    r->hmax_khz = VcrDiagGet(L"MonHmaxKhz", 0);
    r->vmin_hz = VcrDiagGet(L"MonVminHz", 0);
    r->vmax_hz = VcrDiagGet(L"MonVmaxHz", 0);
    r->max_pixclk_khz = VcrDiagGet(L"MonMaxPixclkKhz", 0);
    *id = VcrDiagGet(L"MonId", 0);
}

/* Written only when it changed (a registry write per boot buys nothing), and
 * flushed: the boot that needs it is the one after a power-off with the
 * monitor switched off, and the lazy writer may not have run before that.
 * Returns 1 if it wrote. */
static int mon_save(const vcr_mon_range *r, vcr_u32 id)
{
    vcr_mon_range old;
    vcr_u32 old_id;
    mon_load(&old, &old_id);
    if (old_id == id && old.hmin_khz == r->hmin_khz && old.hmax_khz == r->hmax_khz &&
        old.vmin_hz == r->vmin_hz && old.vmax_hz == r->vmax_hz &&
        old.max_pixclk_khz == r->max_pixclk_khz)
        return 0;
    VcrDiagSet(L"MonHminKhz", r->hmin_khz, FALSE);
    VcrDiagSet(L"MonHmaxKhz", r->hmax_khz, FALSE);
    VcrDiagSet(L"MonVminHz", r->vmin_hz, FALSE);
    VcrDiagSet(L"MonVmaxHz", r->vmax_hz, FALSE);
    VcrDiagSet(L"MonMaxPixclkKhz", r->max_pixclk_khz, FALSE);
    VcrDiagSet(L"MonId", id, TRUE);
    return 1;
}

/* why the persisted values could not stand in (the DEFAULT case's log) */
static const char *default_why(const vcr_edid_info *e, const vcr_mon_range *env, vcr_u32 env_id)
{
    if (!env->hmax_khz)
        return "none saved";
    if (e->valid && env_id != vcr_mon_id(e))
        return "saved is other monitor's";
    return "saved envelope unusable";
}

/* FindAdapter, before the mode list is built. */
void VcrMonitorInit(VCR_EXT *x)
{
    vcr_edid_info *e = &x->mon;
    vcr_mon_range env, before, r;
    vcr_u32 src, env_id, id_before, reset;
    int trusted = 0;
    const char *why = 0;

    x->edid_ok = 0;
    x->mon_src = VCR_MON_SRC_NONE;
    VideoPortZeroMemory(e, sizeof *e);
    vcr_hwcaps_set_range(&x->caps, NULL);                  /* clears the limits */
    if (x->backend != VCR_HW_VOODOO) {
        /* the QEMU bed's display is a window, not a tube: nothing to protect */
        VLOG(VCR_LV_INFO, VCR_EV_EDID, 0, 0, 0, 0, "virtual display: no monitor limits");
        return;
    }
    if (!VcrDiagGet(L"Ddc", 1)) {
        why = "Diag\\Ddc=0";
    } else if (!ddc_read(x, x->edid, sizeof x->edid)) {
        why = "no EDID over DDC";
    } else if (!vcr_edid_parse(x->edid, sizeof x->edid, e)) {
        VLOG(VCR_LV_WARN, VCR_EV_EDID, 0, 0, 0, 0,
             "DDC answered but the block is not an EDID (header/checksum)");
        why = "EDID header/checksum bad";
    } else {
        x->edid_ok = 1;
        VLOG(VCR_LV_INFO, VCR_EV_EDID, vcr_mon_id(e), e->hmin_khz | (e->hmax_khz << 16),
             e->vmin_hz | (e->vmax_hz << 16), e->max_pixclk_khz,
             "monitor %s %s%04x: H %u-%u kHz, V %u-%u Hz, %u MHz%s", e->name, e->pnpid,
             e->product, e->hmin_khz, e->hmax_khz, e->vmin_hz, e->vmax_hz,
             e->max_pixclk_khz / 1000, e->has_range ? "" : " (no range limits)");
        if (!e->has_range)
            why = "EDID has no range limits";
    }

    /* The whole decision is common/vcr_edid.c vcr_mon_boot (tested on the
     * host): MonReset first, then EDID / same monitor / envelope / default,
     * then this EDID's range narrowed into the envelope. */
    mon_load(&env, &env_id);
    before = env;
    id_before = env_id;
    reset = VcrDiagGet(L"MonReset", 0);
    src = vcr_mon_boot(e, reset, &env, &env_id, &r);
    if (mon_save(&env, env_id)) {
        if (!env.hmax_khz)
            VLOG(VCR_LV_INFO, VCR_EV_EDID, 0, 0, 0, 0, "monitor envelope cleared");
        else
            VLOG(VCR_LV_INFO, VCR_EV_EDID, env_id, RANGE_B(env), RANGE_C(env),
                 env.max_pixclk_khz,
                 "envelope (every monitor seen): H %u-%u kHz, V %u-%u Hz, %u MHz",
                 env.hmin_khz, env.hmax_khz, env.vmin_hz, env.vmax_hz,
                 env.max_pixclk_khz / 1000);
        if (env.hmax_khz && !vcr_mon_range_usable(&env))
            VLOG(VCR_LV_WARN, VCR_EV_EDID, env_id, RANGE_B(env), RANGE_C(env),
                 env.max_pixclk_khz,
                 "monitors seen share no range with VGA: a boot without EDID gets the default");
    }
    if (reset) {
        /* after the save, so a power loss between the two resets again
         * rather than keeping the envelope and losing the request */
        VcrDiagSet(L"MonReset", 0, TRUE);
        VLOG(VCR_LV_WARN, VCR_EV_EDID, id_before, RANGE_B(before), RANGE_C(before),
             before.max_pixclk_khz,
             "Diag\\MonReset=1: monitor envelope (a-d: as it was) forgotten, set back to 0");
    }

    /* No EDID: the envelope was bounded by the default, because a tube that
     * cannot answer DDC was never narrowed into it. An operator who knows
     * which tube sits behind the KVM may lift that bound - loudly, every
     * boot: it is the one setting that lets an unread tube be driven past
     * the default. */
    if (src == VCR_MON_SRC_ENVELOPE && VcrDiagGet(L"MonTrustEnvelope", 0)) {
        trusted = vcr_mon_trust_envelope(src, &env, &r);
        if (trusted)
            VLOG(VCR_LV_WARN, VCR_EV_EDID, env_id, RANGE_B(r), RANGE_C(r), r.max_pixclk_khz,
                 "Diag\\MonTrustEnvelope=1: bare envelope, NOT bounded by the default");
    }
    x->mon_src = src;

    /* Every boot the limits are not this EDID's own, say whose they are: a
     * filter nobody can see the source of is one nobody questions when a
     * mode goes missing - or when one is there that should not be. */
    if (!why)
        why = "EDID range unusable";
    if (src == VCR_MON_SRC_DEFAULT)
        VLOG(VCR_LV_WARN, VCR_EV_EDID, env_id, RANGE_B(r), RANGE_C(r), r.max_pixclk_khz,
             "%s -> default (%s): H %u-%u kHz V %u-%u Hz %u MHz", why,
             default_why(e, &env, env_id), r.hmin_khz, r.hmax_khz, r.vmin_hz, r.vmax_hz,
             r.max_pixclk_khz / 1000);
    else if (src != VCR_MON_SRC_EDID)
        VLOG(VCR_LV_WARN, VCR_EV_EDID, env_id, RANGE_B(r), RANGE_C(r), r.max_pixclk_khz,
             "%s -> %s%s: H %u-%u kHz V %u-%u Hz %u MHz", why, vcr_mon_src_name(src),
             src != VCR_MON_SRC_ENVELOPE ? "" : trusted ? " (trusted)" : " bounded by default",
             r.hmin_khz, r.hmax_khz, r.vmin_hz, r.vmax_hz, r.max_pixclk_khz / 1000);

    if (!VcrDiagGet(L"EdidFilter", 1)) {
        x->mon_src = VCR_MON_SRC_NONE;          /* nothing is in force */
        VLOG(VCR_LV_WARN, VCR_EV_EDID, vcr_mon_id(e), RANGE_B(r), RANGE_C(r), r.max_pixclk_khz,
             "Diag\\EdidFilter=0: every mode listed, NOT limited to the monitor (override)");
        return;
    }
    vcr_hwcaps_set_range(&x->caps, &r);
}

/* HwGetVideoChildDescriptor: index 1 is the monitor. */
VP_STATUS VcrMonitorChild(VCR_EXT *x, PVIDEO_CHILD_ENUM_INFO ci, PVIDEO_CHILD_TYPE type,
                          PUCHAR desc, PULONG uid)
{
    ULONG n = 0;
    if (ci->ChildIndex != 1)
        return ERROR_NO_MORE_DEVICES;
    *type = Monitor;
    *uid = 0x100;
    if (desc && ci->ChildDescriptorSize) {
        VideoPortZeroMemory(desc, ci->ChildDescriptorSize);
        if (x->edid_ok) {
            n = ci->ChildDescriptorSize < sizeof x->edid ? ci->ChildDescriptorSize
                                                          : sizeof x->edid;
            VideoPortMoveMemory(desc, x->edid, n);
        }
    }
    VLOG(VCR_LV_INFO, VCR_EV_CHILD, ci->ChildIndex, VIDEO_ENUM_MORE_DEVICES, n,
         ci->ChildDescriptorSize, "monitor child, %u EDID bytes%s", n,
         n ? "" : " (XP will call it a Default Monitor)");
    return VIDEO_ENUM_MORE_DEVICES;
}
