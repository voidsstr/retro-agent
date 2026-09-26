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
 * to XP as the monitor child's descriptor. No EDID = no filter, logged.
 * Diag\Ddc = 0 skips the read; Diag\EdidFilter = 0 keeps the EDID but lists
 * every mode.
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

/* FindAdapter, before the mode list is built. */
void VcrMonitorInit(VCR_EXT *x)
{
    vcr_edid_info *e = &x->mon;

    x->edid_ok = 0;
    VideoPortZeroMemory(e, sizeof *e);
    vcr_hwcaps_set_monitor(&x->caps, e);                   /* clears the limits */
    if (x->backend != VCR_HW_VOODOO || !VcrDiagGet(L"Ddc", 1))
        return;
    if (!ddc_read(x, x->edid, sizeof x->edid))
        return;
    if (!vcr_edid_parse(x->edid, sizeof x->edid, e)) {
        VLOG(VCR_LV_WARN, VCR_EV_EDID, 0, 0, 0, 0,
             "DDC answered but the block is not an EDID (header/checksum) - modes unfiltered");
        return;
    }
    x->edid_ok = 1;
    VLOG(VCR_LV_INFO, VCR_EV_EDID,
         (((ULONG)(e->pnpid[0] - '@') << 10) | ((ULONG)(e->pnpid[1] - '@') << 5) |
          (ULONG)(e->pnpid[2] - '@')) | ((ULONG)e->product << 16),
         e->hmin_khz | (e->hmax_khz << 16), e->vmin_hz | (e->vmax_hz << 16), e->max_pixclk_khz,
         "monitor %s %s%04x: H %u-%u kHz, V %u-%u Hz, %u MHz%s", e->name, e->pnpid,
         e->product, e->hmin_khz, e->hmax_khz, e->vmin_hz, e->vmax_hz,
         e->max_pixclk_khz / 1000, e->has_range ? "" : " (no range limits: modes unfiltered)");
    if (VcrDiagGet(L"EdidFilter", 1))
        vcr_hwcaps_set_monitor(&x->caps, e);
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
