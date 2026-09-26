/*
 * vcrmp_hw.c - hardware access for the vcr-kmd miniport.
 *
 * Two backends behind one interface:
 *   VCR_HW_VOODOO - Banshee / Voodoo 3 / VSA-100. 32-bit registers through the
 *                   kernel mapping of memBase0; VGA registers through the I/O
 *                   BAR (offsets 0xb0-0xdf = ports 0x3b0-0x3df), which is how
 *                   Glide's own cinit reaches them.
 *   VCR_HW_BOCHS  - QEMU std-vga (1234:1111): the Bochs dispi interface at
 *                   BAR2+0x500. It exists to prove the whole driver chassis in
 *                   a VM before anything runs on the Voodoo.
 *
 * Rules this file keeps, each paid for elsewhere on this project:
 *   - every poll loop is bounded and logs VCR_EV_TIMEOUT with the state it
 *     saw (a PCI read that never completes is the one hang nothing catches);
 *   - every mode-set register that matters is READ BACK and logged, so a
 *     mismatch is a log line rather than a black screen with no explanation.
 */
#include "vcrmp.h"
#include "../include/vcr_pciraw.h"

/* ---- register access ----------------------------------------------------------- */

ULONG VcrRd(VCR_EXT *x, ULONG chip, ULONG off)
{
    if (chip >= x->nchips || !x->chip[chip].regs)
        return 0xffffffffu;
    return VideoPortReadRegisterUlong((PULONG)(x->chip[chip].regs + off));
}

void VcrWr(VCR_EXT *x, ULONG chip, ULONG off, ULONG v)
{
    if (chip >= x->nchips || !x->chip[chip].regs)
        return;
    VideoPortWriteRegisterUlong((PULONG)(x->chip[chip].regs + off), v);
}

static PUCHAR vga_port(VCR_EXT *x, ULONG port)
{
    if (x->backend == VCR_HW_BOCHS)
        return x->bochs_vga ? x->bochs_vga + (port - 0x3c0) : NULL;
    return x->io ? x->io + VCR_VGA(port) : NULL;
}

UCHAR VcrVgaRd(VCR_EXT *x, ULONG port)
{
    PUCHAR p = vga_port(x, port);
    if (!p)
        return 0xff;
    return x->backend == VCR_HW_BOCHS ? VideoPortReadRegisterUchar(p)
                                      : VideoPortReadPortUchar(p);
}

void VcrVgaWr(VCR_EXT *x, ULONG port, UCHAR v)
{
    PUCHAR p = vga_port(x, port);
    if (!p)
        return;
    if (x->backend == VCR_HW_BOCHS)
        VideoPortWriteRegisterUchar(p, v);
    else
        VideoPortWritePortUchar(p, v);
}

UCHAR VcrVgaIdxRd(VCR_EXT *x, ULONG port, UCHAR idx)
{
    VcrVgaWr(x, port, idx);
    return VcrVgaRd(x, port + 1);
}

void VcrVgaIdxWr(VCR_EXT *x, ULONG port, UCHAR idx, UCHAR v)
{
    VcrVgaWr(x, port, idx);
    VcrVgaWr(x, port + 1, v);
}

UCHAR VcrAttrRd(VCR_EXT *x, UCHAR idx)
{
    UCHAR v;
    (void)VcrVgaRd(x, VCR_VGA_IS1_R);           /* reset the index/data flip-flop */
    VcrVgaWr(x, VCR_VGA_ATTR_W, (UCHAR)(idx | 0x20));   /* keep the screen on */
    v = VcrVgaRd(x, VCR_VGA_ATTR_R);
    (void)VcrVgaRd(x, VCR_VGA_IS1_R);
    return v;
}

static void attr_wr(VCR_EXT *x, UCHAR idx, UCHAR v)
{
    (void)VcrVgaRd(x, VCR_VGA_IS1_R);
    VcrVgaWr(x, VCR_VGA_ATTR_W, idx);
    VcrVgaWr(x, VCR_VGA_ATTR_W, v);
}

/* ---- PCI config space ---------------------------------------------------------
 * Function 0 (the chip PnP gave us): the HAL. Functions 1-7: RAW mechanism #1
 * cycles, because the HAL does not probe them - the V5 6000's function-0
 * header has no multifunction bit, and HalGetBusDataByOffset reports its
 * three slave chips absent (measured on .124 with vcrprobe: raw cycles find
 * all four at bus 3 dev 0 fn 0-3). videoprt's VideoPortGetBusData is no help
 * either: for a PnP device it substitutes the adapter's own slot.
 * Raw cycles run with interrupts off; that does not exclude the HAL on
 * another CPU, which is acceptable for a UP box and rare config traffic. */

ULONG VcrPciSlot(ULONG dev, ULONG fn)
{
    return (dev & 0x1f) | ((fn & 7) << 5);
}

static ULONG raw_read(VCR_EXT *x, ULONG slot, ULONG off, ULONG size)
{
    ULONG d = vcr_pci_raw_read32(x->bus, slot & 0x1f, (slot >> 5) & 7, off & ~3u);
    ULONG sh = (off & 3) * 8;
    if (size == 4)
        return d;
    return (d >> sh) & (size == 1 ? 0xffu : 0xffffu);
}

ULONG VcrPciRead(VCR_EXT *x, ULONG slot, ULONG off, ULONG size)
{
    ULONG v = 0;
    if ((slot >> 5) & 7)
        return raw_read(x, slot, off, size);
    if (HalGetBusDataByOffset(VCR_PCIConfiguration, x->bus, slot, &v, off, size) != size)
        return 0xffffffffu;
    return v;
}

void VcrPciWrite(VCR_EXT *x, ULONG slot, ULONG off, ULONG v, ULONG size)
{
    if ((slot >> 5) & 7) {
        ULONG d = v, sh = (off & 3) * 8;
        if (size != 4) {
            ULONG m = (size == 1 ? 0xffu : 0xffffu) << sh;
            d = (vcr_pci_raw_read32(x->bus, slot & 0x1f, (slot >> 5) & 7, off & ~3u) & ~m) |
                ((v << sh) & m);
        }
        vcr_pci_raw_write32(x->bus, slot & 0x1f, (slot >> 5) & 7, off & ~3u, d);
        return;
    }
    HalSetBusDataByOffset(VCR_PCIConfiguration, x->bus, slot, &v, off, size);
}

static ULONG pci_read_bus(ULONG bus, ULONG slot, ULONG off)
{
    ULONG v = 0xffffffffu;
    if (HalGetBusDataByOffset(VCR_PCIConfiguration, bus, slot, &v, off, 4) != 4)
        return 0xffffffffu;
    return v;
}

/* ---- idle ------------------------------------------------------------------------ */

ULONG VcrHwWaitIdle(VCR_EXT *x, ULONG chip, ULONG loops)
{
    ULONG i, st = 0, idle_seen = 0;
    if (x->backend != VCR_HW_VOODOO)
        return 1;
    for (i = 0; i < loops; i++) {
        st = VcrRd(x, chip, VCR_R_STATUS);
        if (st == 0xffffffffu)
            break;                              /* the bus answered nothing */
        if (!(st & VCR_STATUS_BUSY)) {
            if (++idle_seen >= 3)               /* the vendor code wants it seen 3x */
                return 1;
        } else {
            idle_seen = 0;
        }
    }
    VLOG(VCR_LV_WARN, VCR_EV_IDLE_WAIT, st, i, 1, chip, "chip %u not idle", chip);
    return 0;
}

/* ---- discovery --------------------------------------------------------------------- */

static ULONG voodoo_fb_bytes(VCR_EXT *x)
{
    ULONG di0 = VcrRd(x, 0, VCR_R_DRAMINIT0);
    ULONG di1 = VcrRd(x, 0, VCR_R_DRAMINIT1);
    ULONG parts = (di0 & VCR_DI0_SGRAM_NUM_CHIPSETS) ? 8 : 4;
    ULONG part_mb, bytes;

    if (VCR_IS_NAPALM(x->device)) {
        part_mb = 1u << ((di0 & VCR_DI0_H5_SGRAM_TYPE_MASK) >> VCR_DI0_SGRAM_TYPE_SHIFT);
    } else {
        ULONG sgram = !(di1 & VCR_DI1_MCTL_TYPE_SDRAM);
        part_mb = 2;
        if (sgram && !(di0 & VCR_DI0_H4_SGRAM_TYPE))
            part_mb = 1;
    }
    bytes = parts * part_mb * 1024u * 1024u;
    VLOG(VCR_LV_INFO, VCR_EV_MEMSIZE, di0, di1, bytes, x->lfb_len,
         "fb %u MB (%u parts x %u MB), BAR1 %u MB", bytes >> 20, parts, part_mb,
         x->lfb_len >> 20);
    /* never claim more than half the LFB aperture (the upper half is tiled) */
    if (x->lfb_len && bytes > x->lfb_len / 2 && x->lfb_len >= 2 * (4u << 20))
        bytes = x->lfb_len / 2;
    return bytes;
}

static void find_slaves(VCR_EXT *x)
{
    ULONG dev = x->slot & 0x1f, fn, id;
    x->nchips = 1;
    for (fn = 1; fn < 8 && x->nchips < VCR_MAX_CHIPS; fn++) {
        ULONG slot = VcrPciSlot(dev, fn);
        id = VcrPciRead(x, slot, 0, 4);
        if (id == 0xffffffffu || (id & 0xffff) != x->vendor || (id >> 16) != x->device)
            continue;
        x->chip[x->nchips].slot = slot;
        x->chip[x->nchips].mmio_phys.QuadPart = VcrPciRead(x, slot, 0x10, 4) & ~0xfu;
        x->chip[x->nchips].lfb_phys.QuadPart = VcrPciRead(x, slot, 0x14, 4) & ~0xfu;
        x->chip[x->nchips].io_phys = VcrPciRead(x, slot, 0x18, 4) & ~0x3u;
        VLOG(VCR_LV_INFO, VCR_EV_CHIP_FOUND, x->nchips, (x->bus << 8) | slot,
             x->chip[x->nchips].mmio_phys.LowPart, x->chip[x->nchips].lfb_phys.LowPart,
             "slave chip %u at %u:%u.%u (BARs as found, not programmed)",
             x->nchips, x->bus, dev, fn);
        x->nchips++;
    }
}

static void find_hint_bridge(VCR_EXT *x)
{
    ULONG bus, dev;
    for (bus = 0; bus < x->bus + 1 && bus < 8; bus++)
        for (dev = 0; dev < 32; dev++) {
            ULONG id = pci_read_bus(bus, VcrPciSlot(dev, 0), 0);
            if (id == ((VCR_DEV_HINT_HB1 << 16) | VCR_PCI_VENDOR_HINT)) {
                ULONG buses = pci_read_bus(bus, VcrPciSlot(dev, 0), 0x18);
                VLOG(VCR_LV_INFO, VCR_EV_BRIDGE_FOUND, bus, dev, id, buses,
                     "HiNT HB1 bridge %u:%u (secondary bus %u)", bus, dev,
                     (buses >> 8) & 0xff);
                if (((buses >> 8) & 0xff) == x->bus) {
                    x->bridge_bus = bus;
                    x->bridge_slot = VcrPciSlot(dev, 0);
                    x->bridge_found = 1;
                }
            }
        }
}

VP_STATUS VcrHwDiscover(VCR_EXT *x)
{
    ULONG over;
    if (x->backend == VCR_HW_BOCHS) {
        ULONG id = x->dispi[0];
        x->nchips = 1;
        x->bochs_vram = (ULONG)x->dispi[0xa] << 16;
        if (!x->bochs_vram)
            x->bochs_vram = x->lfb_len;
        x->fb_per_chip = x->bochs_vram;
        x->caps.device_id = 0;
        x->caps.max_pixclk_khz = 400000;
        x->caps.twox_above_khz = 400000;
        x->caps.fb_bytes = x->fb_per_chip;
        x->caps.fb_reserved = 0;
        x->desktop_offset = 0;
        VLOG(VCR_LV_INFO, VCR_EV_MEMSIZE, id, 0, x->fb_per_chip, x->lfb_len,
             "bochs dispi id %x, %u MB", id, x->fb_per_chip >> 20);
        return (id >= 0xb0c0 && id <= 0xb0cf) ? NO_ERROR : ERROR_DEV_NOT_EXIST;
    }

    find_slaves(x);
    find_hint_bridge(x);
    x->fb_per_chip = voodoo_fb_bytes(x);

    x->caps.device_id = x->device;
    x->caps.max_pixclk_khz = VCR_IS_NAPALM(x->device) ? 350000
                             : x->device == VCR_DEV_VOODOO3 ? 300000 : 270000;
    /* 2X mode, the vendor's rule (H5 h3modeset.c; golden capture: dacMode 0
     * at every mode .124's monitor offers) - see vcr_mode_compute(). */
    x->caps.twox_above_khz = VCR_IS_NAPALM(x->device) ? 262000 : 160000;
    x->caps.twox_htotal_chars = VCR_IS_NAPALM(x->device) ? 261 : 0;
    x->caps.fb_bytes = x->fb_per_chip;
    /* The desktop starts 1 MB in: Glide keeps its command FIFO at 96 KB, so a
     * GDI write that lands while a game owns the chip can at worst touch a
     * texture, never the FIFO. */
    x->desktop_offset = VcrDiagGet(L"DesktopOffset", 0x100000) & ~0xfffu;
    x->caps.fb_reserved = x->desktop_offset + (64u << 10);
    /* Overrides, so a golden-capture disagreement is fixed by a registry value
     * on the box before it is fixed in the table. */
    if ((over = VcrDiagGet(L"MaxPixclkKhz", 0)) != 0)
        x->caps.max_pixclk_khz = over;
    if ((over = VcrDiagGet(L"TwoXAboveKhz", 0)) != 0)
        x->caps.twox_above_khz = over;
    x->caps.napalm_vpc_extra = VcrDiagGet(L"NapalmVpcExtra", 0);
    x->lfbmemcfg_linear = VcrDiagGet(L"LfbMemoryConfig", 0x01803fff);
    return NO_ERROR;
}

/* ---- snapshots ----------------------------------------------------------------------- */

static const struct { USHORT off; const char *name; } k_regs[] = {
    { VCR_R_STATUS, "status" },           { VCR_R_PCIINIT0, "pciInit0" },
    { VCR_R_LFBMEMORYCONFIG, "lfbMemCfg" }, { VCR_R_MISCINIT0, "miscInit0" },
    { VCR_R_MISCINIT1, "miscInit1" },     { VCR_R_DRAMINIT0, "dramInit0" },
    { VCR_R_DRAMINIT1, "dramInit1" },     { VCR_R_AGPINIT, "agpInit" },
    { VCR_R_TMUGBEINIT, "tmuGbeInit" },   { VCR_R_VGAINIT0, "vgaInit0" },
    { VCR_R_VGAINIT1, "vgaInit1" },       { VCR_R_STRAPINFO, "strapInfo" },
    { VCR_R_PLLCTRL0, "pllCtrl0" },       { VCR_R_PLLCTRL1, "pllCtrl1" },
    { VCR_R_PLLCTRL2, "pllCtrl2" },       { VCR_R_DACMODE, "dacMode" },
    { VCR_R_VIDPROCCFG, "vidProcCfg" },   { VCR_R_VIDSCREENSIZE, "vidScreenSize" },
    { VCR_R_VIDDESKTOPSTARTADDR, "deskStart" },
    { VCR_R_VIDDESKTOPOVERLAYSTRIDE, "deskStride" },
    { VCR_R_VIDPIXELBUFTHOLD, "pixBufThold" },
};

void VcrHwSnapshotToLog(VCR_EXT *x, const char *why)
{
    ULONG i, c;
    if (x->backend != VCR_HW_VOODOO)
        return;
    for (c = 0; c < x->nchips; c++) {
        if (!x->chip[c].regs)
            continue;
        for (i = 0; i < sizeof k_regs / sizeof k_regs[0]; i++)
            VLOG(VCR_LV_DEBUG, VCR_EV_REG_SNAPSHOT, k_regs[i].off,
                 VcrRd(x, c, k_regs[i].off), c, 0, "%s chip%u %s", why, c, k_regs[i].name);
    }
    VLOG(VCR_LV_DEBUG, VCR_EV_REG_SNAPSHOT, 0x1a, VcrVgaIdxRd(x, VCR_VGA_CRTC_I, 0x1a),
         0, 0x3d4, "%s CR1A", why);
    VLOG(VCR_LV_DEBUG, VCR_EV_REG_SNAPSHOT, 0x1b, VcrVgaIdxRd(x, VCR_VGA_CRTC_I, 0x1b),
         0, 0x3d4, "%s CR1B", why);
}

void VcrHwSaveBootState(VCR_EXT *x)
{
    VCR_BOOTSTATE *b = &x->boot;
    ULONG i;
    if (x->backend != VCR_HW_VOODOO)
        return;
    b->vidproccfg = VcrRd(x, 0, VCR_R_VIDPROCCFG);
    b->dacmode = VcrRd(x, 0, VCR_R_DACMODE);
    b->pllctrl0 = VcrRd(x, 0, VCR_R_PLLCTRL0);
    b->vgainit0 = VcrRd(x, 0, VCR_R_VGAINIT0);
    b->vgainit1 = VcrRd(x, 0, VCR_R_VGAINIT1);
    b->miscinit0 = VcrRd(x, 0, VCR_R_MISCINIT0);
    b->lfbmemcfg = VcrRd(x, 0, VCR_R_LFBMEMORYCONFIG);
    b->desktopstart = VcrRd(x, 0, VCR_R_VIDDESKTOPSTARTADDR);
    b->stride = VcrRd(x, 0, VCR_R_VIDDESKTOPOVERLAYSTRIDE);
    b->screensize = VcrRd(x, 0, VCR_R_VIDSCREENSIZE);
    b->misc = VcrVgaRd(x, VCR_VGA_MISC_R);
    for (i = 0; i < sizeof b->crtc; i++)
        b->crtc[i] = VcrVgaIdxRd(x, VCR_VGA_CRTC_I, (UCHAR)i);
    for (i = 0; i < sizeof b->seq; i++)
        b->seq[i] = VcrVgaIdxRd(x, VCR_VGA_SEQ_I, (UCHAR)i);
    for (i = 0; i < sizeof b->gfx; i++)
        b->gfx[i] = VcrVgaIdxRd(x, VCR_VGA_GFX_I, (UCHAR)i);
    for (i = 0; i < sizeof b->attr; i++)
        b->attr[i] = VcrAttrRd(x, (UCHAR)i);
    b->saved = 1;
}

void VcrHwSnapshot(VCR_EXT *x, vcr_snapshot *s)
{
    ULONG c, i;
    VideoPortZeroMemory(s, sizeof *s);
    s->nchips = x->nchips;
    for (c = 0; c < x->nchips && c < VCR_MAX_CHIPS; c++) {
        vcr_chip_snapshot *k = &s->chip[c];
        for (i = 0; i < 64; i++) {
            k->ioregs[i] = x->chip[c].regs ? VcrRd(x, c, i * 4) : 0xffffffffu;
            k->cfg[i] = VcrPciRead(x, x->chip[c].slot, i * 4, 4);
        }
        if (c != 0)
            continue;       /* the I/O BAR decodes for the master only */
        k->misc = VcrVgaRd(x, VCR_VGA_MISC_R);
        for (i = 0; i < sizeof k->crtc; i++)
            k->crtc[i] = VcrVgaIdxRd(x, VCR_VGA_CRTC_I, (UCHAR)i);
        for (i = 0; i < sizeof k->seq; i++)
            k->seq[i] = VcrVgaIdxRd(x, VCR_VGA_SEQ_I, (UCHAR)i);
        for (i = 0; i < sizeof k->gfx; i++)
            k->gfx[i] = VcrVgaIdxRd(x, VCR_VGA_GFX_I, (UCHAR)i);
        for (i = 0; i < sizeof k->attr; i++)
            k->attr[i] = VcrAttrRd(x, (UCHAR)i);
    }
}

/* ---- CLUT -------------------------------------------------------------------------- */

/* xf86-video-tdfx retries both writes until they read back (up to 100x): the
 * DAC address/data pair is known to drop writes. Count the retries - a box that
 * needs them is telling us something about its bus. */
static ULONG clut_write(VCR_EXT *x, ULONG index, ULONG rgb)
{
    ULONG tries = 0;
    do {
        VcrWr(x, 0, VCR_R_DACADDR, index);
    } while (VcrRd(x, 0, VCR_R_DACADDR) != index && ++tries < 100);
    do {
        VcrWr(x, 0, VCR_R_DACDATA, rgb);
    } while (VcrRd(x, 0, VCR_R_DACDATA) != rgb && ++tries < 200);
    return tries;
}

static void clut_identity(VCR_EXT *x)
{
    ULONG i, retries = 0;
    for (i = 0; i < 512; i++) {
        ULONG v = i & 0xff;
        retries += clut_write(x, i, (v << 16) | (v << 8) | v);
    }
    VLOG(retries ? VCR_LV_WARN : VCR_LV_DEBUG, VCR_EV_PALETTE, 0, 512, retries, 0,
         "identity CLUT (both banks), %u retries", retries);
}

VP_STATUS VcrHwSetClut(VCR_EXT *x, const VIDEO_CLUT *clut, ULONG len)
{
    ULONG i, retries = 0;
    if (len < sizeof(VIDEO_CLUT) - sizeof(ULONG) ||
        len < sizeof(VIDEO_CLUT) - sizeof(ULONG) + clut->NumEntries * sizeof(ULONG) ||
        clut->FirstEntry + clut->NumEntries > 256)
        return ERROR_INVALID_PARAMETER;
    for (i = 0; i < clut->NumEntries; i++) {
        const VIDEO_CLUTDATA *d = &clut->LookupTable[i].RgbArray;
        ULONG idx = clut->FirstEntry + i;
        if (x->backend == VCR_HW_BOCHS) {
            VcrVgaWr(x, VCR_VGA_DAC_WI, (UCHAR)idx);
            VcrVgaWr(x, VCR_VGA_DAC_D, d->Red);
            VcrVgaWr(x, VCR_VGA_DAC_D, d->Green);
            VcrVgaWr(x, VCR_VGA_DAC_D, d->Blue);
        } else {
            retries += clut_write(x, idx, ((ULONG)d->Red << 16) |
                                          ((ULONG)d->Green << 8) | d->Blue);
        }
    }
    VLOG(retries ? VCR_LV_WARN : VCR_LV_TRACE, VCR_EV_PALETTE, clut->FirstEntry,
         clut->NumEntries, retries, 0, "palette");
    return NO_ERROR;
}

/* ---- mode set ------------------------------------------------------------------------ */

static void verify(VCR_EXT *x, ULONG off, ULONG want, ULONG mask, const char *name)
{
    ULONG got = VcrRd(x, 0, off);
    ULONG bad = (got & mask) != (want & mask);
    VLOG(bad ? VCR_LV_WARN : VCR_LV_DEBUG, VCR_EV_MODESET_REG, off, want, got, bad,
         "%s %s", name, bad ? "MISMATCH" : "ok");
}

static void verify_crtc(VCR_EXT *x, UCHAR idx, UCHAR want)
{
    UCHAR got = VcrVgaIdxRd(x, VCR_VGA_CRTC_I, idx);
    VLOG(got != want ? VCR_LV_WARN : VCR_LV_DEBUG, VCR_EV_MODESET_REG, 0x300 | idx,
         want, got, got != want, "CR%02x %s", idx, got != want ? "MISMATCH" : "ok");
}

static VP_STATUS voodoo_program(VCR_EXT *x, const vcr_modeset *m)
{
    ULONG i, v;

    VcrHwWaitIdle(x, 0, 200000);

    VcrWr(x, 0, VCR_R_MISCINIT1, VcrRd(x, 0, VCR_R_MISCINIT1) | VCR_MI1_CLUT_INVERT);
    /* keep VGA decode and the VGA base-address bits; set the extension bits */
    v = (VcrRd(x, 0, VCR_R_VGAINIT0) & (VCR_VGA0_LEGACY_DECODE | 0xffffc000u)) |
        m->vgainit0_set;
    VcrWr(x, 0, VCR_R_VGAINIT0, v);
    VcrVgaWr(x, VCR_VGA_WAKEUP, 1);
    VcrVgaIdxWr(x, VCR_VGA_CRTC_I, 0x11,
                (UCHAR)(VcrVgaIdxRd(x, VCR_VGA_CRTC_I, 0x11) & 0x7f));
    VcrWr(x, 0, VCR_R_VGAINIT1, VcrRd(x, 0, VCR_R_VGAINIT1) & 0x001fffffu);
    VcrWr(x, 0, VCR_R_VIDPROCCFG, m->vidproccfg & ~VCR_VPC_VIDEO_PROCESSOR_EN);
    VcrWr(x, 0, VCR_R_PLLCTRL0, m->pllctrl0);

    VcrVgaWr(x, VCR_VGA_MISC_W, m->misc);
    VcrVgaIdxWr(x, VCR_VGA_SEQ_I, 0, 0x01);         /* synchronous reset */
    for (i = 1; i < 5; i++)
        VcrVgaIdxWr(x, VCR_VGA_SEQ_I, (UCHAR)i, m->seq[i]);
    VcrVgaIdxWr(x, VCR_VGA_SEQ_I, 0, m->seq[0]);
    for (i = 0; i < 25; i++)
        VcrVgaIdxWr(x, VCR_VGA_CRTC_I, (UCHAR)i, m->crtc[i]);
    for (i = 0; i < 9; i++)
        VcrVgaIdxWr(x, VCR_VGA_GFX_I, (UCHAR)i, m->gfx[i]);
    for (i = 0; i < 21; i++)
        attr_wr(x, (UCHAR)i, m->attr[i]);
    (void)VcrVgaRd(x, VCR_VGA_IS1_R);
    VcrVgaWr(x, VCR_VGA_ATTR_W, 0x20);              /* palette on, video on */
    VcrVgaIdxWr(x, VCR_VGA_CRTC_I, 0x1a, m->crtc_ext[0]);
    VcrVgaIdxWr(x, VCR_VGA_CRTC_I, 0x1b, m->crtc_ext[1]);

    VcrWr(x, 0, VCR_R_DACMODE, m->dacmode);
    VcrWr(x, 0, VCR_R_VIDDESKTOPOVERLAYSTRIDE, m->stride);
    VcrWr(x, 0, VCR_R_VIDSCREENSIZE, m->vidscreensize);
    VcrWr(x, 0, VCR_R_VIDPIXELBUFTHOLD, 0x00010410);    /* vendor value, every mode */
    VcrWr(x, 0, VCR_R_VIDDESKTOPSTARTADDR, x->desktop_offset);
    /* Glide re-tiles the LFB and moves the Y origin; a linear desktop wants
     * the tile aperture pushed past the end of memory and no Y flip - the
     * vendor's own values for its linear (8 bpp) desktops, golden capture
     * (Diag\\LfbMemoryConfig overrides). */
    VcrWr(x, 0, VCR_R_LFBMEMORYCONFIG, x->lfbmemcfg_linear);
    VcrWr(x, 0, VCR_R_MISCINIT0, 0);
    clut_identity(x);
    VcrWr(x, 0, VCR_R_VIDPROCCFG, m->vidproccfg);

    verify(x, VCR_R_VIDPROCCFG, m->vidproccfg, 0xffffffffu, "vidProcCfg");
    verify(x, VCR_R_PLLCTRL0, m->pllctrl0, 0xffffu, "pllCtrl0");
    verify(x, VCR_R_DACMODE, m->dacmode, 0x1f, "dacMode");
    verify(x, VCR_R_VIDSCREENSIZE, m->vidscreensize, 0x00ffffffu, "vidScreenSize");
    verify(x, VCR_R_VIDDESKTOPOVERLAYSTRIDE, m->stride, 0x7fff, "deskStride");
    verify_crtc(x, 0x00, m->crtc[0x00]);
    verify_crtc(x, 0x06, m->crtc[0x06]);
    verify_crtc(x, 0x1a, m->crtc_ext[0]);
    verify_crtc(x, 0x1b, m->crtc_ext[1]);
    return NO_ERROR;
}

#define DISPI(x, i, v) ((x)->dispi[(i)] = (USHORT)(v))
static VP_STATUS bochs_program(VCR_EXT *x, const vcr_timing *t, ULONG bpp)
{
    DISPI(x, 4, 0);                                 /* ENABLE off */
    DISPI(x, 1, t->w);
    DISPI(x, 2, t->h);
    DISPI(x, 3, bpp);
    DISPI(x, 6, t->w);                              /* virtual width */
    DISPI(x, 7, t->h);
    DISPI(x, 8, 0);
    DISPI(x, 9, 0);
    DISPI(x, 4, 0x01 | 0x20 | 0x40);                /* on, 8-bit DAC, LFB */
    VLOG(VCR_LV_DEBUG, VCR_EV_MODESET_REG, 1, t->w, x->dispi[1], x->dispi[1] != t->w,
         "bochs xres");
    VLOG(VCR_LV_DEBUG, VCR_EV_MODESET_REG, 3, bpp, x->dispi[3], x->dispi[3] != bpp,
         "bochs bpp");
    return NO_ERROR;
}

VP_STATUS VcrHwSetMode(VCR_EXT *x, ULONG idx)
{
    const vcr_timing *t;
    vcr_modeset m;
    ULONG bpp, t0 = VcrMs();
    int rc;
    VP_STATUS st;

    if (idx >= x->nmodes)
        return ERROR_INVALID_PARAMETER;
    t = &vcr_timings[x->modes[idx].timing];
    bpp = x->modes[idx].bpp;
    rc = vcr_mode_compute(&x->caps, t, bpp, &m);
    if (rc) {
        VLOG(VCR_LV_ERROR, VCR_EV_MODESET_FAIL, idx, rc, 0, 0, "compute refused %ux%u",
             t->w, t->h);
        return ERROR_INVALID_PARAMETER;
    }
    VcrPhase(VCR_EV_MODESET_BEGIN, (t->w << 16) | t->h, (bpp << 16) | t->refresh,
             "mode set");
    VLOG(VCR_LV_INFO, VCR_EV_MODESET_BEGIN, t->w, t->h, bpp, t->refresh,
         "mode %u: %ux%ux%u@%u", idx, t->w, t->h, bpp, t->refresh);
    VLOG(VCR_LV_INFO, VCR_EV_MODESET_PLL, m.pix_khz_target, m.pix_khz_actual, m.pllctrl0,
         m.twox, "pll %u kHz -> %u kHz, refresh %u mHz", m.pix_khz_target,
         m.pix_khz_actual, m.refresh_mhz);

    st = x->backend == VCR_HW_BOCHS ? bochs_program(x, t, bpp) : voodoo_program(x, &m);
    if (st != NO_ERROR)
        return st;
    x->cur_mode = (LONG)idx;
    x->cur_set = m;
    x->cur_stride = m.stride;
    VLOG(VCR_LV_INFO, VCR_EV_MODESET_DONE, m.vidproccfg, m.vidscreensize, m.stride,
         (VcrMs() - t0) * 1000, "mode set done");
    VcrPhase(VCR_EV_MODESET_DONE, idx, 0, "mode set done");
    return NO_ERROR;
}

VP_STATUS VcrHwRestoreMode(VCR_EXT *x)
{
    if (x->cur_mode < 0)
        return ERROR_INVALID_PARAMETER;
    VLOG(VCR_LV_INFO, VCR_EV_VGA_RESTORE, x->cur_mode, 0, 0, 0,
         "re-programming mode %d after an exclusive owner", x->cur_mode);
    return VcrHwSetMode(x, (ULONG)x->cur_mode);
}

/* Back to what the BIOS left. Called from HwResetHw at bugcheck/shutdown time
 * (any IRQL, no allocation, no registry) and from RESET_DEVICE. */
void VcrHwResetToVga(VCR_EXT *x)
{
    VCR_BOOTSTATE *b = &x->boot;
    ULONG i;
    if (x->backend == VCR_HW_BOCHS) {
        if (x->dispi)
            DISPI(x, 4, 0);
        x->cur_mode = -1;
        return;
    }
    if (!b->saved || !x->chip[0].regs)
        return;
    VcrWr(x, 0, VCR_R_VIDPROCCFG, b->vidproccfg & ~VCR_VPC_VIDEO_PROCESSOR_EN);
    VcrWr(x, 0, VCR_R_DACMODE, b->dacmode);
    VcrWr(x, 0, VCR_R_PLLCTRL0, b->pllctrl0);
    VcrWr(x, 0, VCR_R_VGAINIT0, b->vgainit0);
    VcrWr(x, 0, VCR_R_VGAINIT1, b->vgainit1);
    VcrWr(x, 0, VCR_R_LFBMEMORYCONFIG, b->lfbmemcfg);
    VcrWr(x, 0, VCR_R_MISCINIT0, b->miscinit0);
    VcrWr(x, 0, VCR_R_VIDSCREENSIZE, b->screensize);
    VcrWr(x, 0, VCR_R_VIDDESKTOPOVERLAYSTRIDE, b->stride);
    VcrWr(x, 0, VCR_R_VIDDESKTOPSTARTADDR, b->desktopstart);
    VcrVgaWr(x, VCR_VGA_MISC_W, b->misc);
    VcrVgaIdxWr(x, VCR_VGA_SEQ_I, 0, 0x01);
    for (i = 1; i < sizeof b->seq; i++)
        VcrVgaIdxWr(x, VCR_VGA_SEQ_I, (UCHAR)i, b->seq[i]);
    VcrVgaIdxWr(x, VCR_VGA_SEQ_I, 0, b->seq[0]);
    VcrVgaIdxWr(x, VCR_VGA_CRTC_I, 0x11, (UCHAR)(b->crtc[0x11] & 0x7f));
    for (i = 0; i < sizeof b->crtc; i++)
        VcrVgaIdxWr(x, VCR_VGA_CRTC_I, (UCHAR)i, b->crtc[i]);
    for (i = 0; i < sizeof b->gfx; i++)
        VcrVgaIdxWr(x, VCR_VGA_GFX_I, (UCHAR)i, b->gfx[i]);
    for (i = 0; i < sizeof b->attr; i++)
        attr_wr(x, (UCHAR)i, b->attr[i]);
    (void)VcrVgaRd(x, VCR_VGA_IS1_R);
    VcrVgaWr(x, VCR_VGA_ATTR_W, 0x20);
    VcrWr(x, 0, VCR_R_VIDPROCCFG, b->vidproccfg);
    x->cur_mode = -1;
}

/* DPMS through dacMode, as tdfxfb blanks: bit 1 / bit 3 hold the syncs. */
void VcrHwPower(VCR_EXT *x, ULONG state)
{
    ULONG d, bits;
    if (x->backend != VCR_HW_VOODOO)
        return;
    bits = state == VideoPowerOn ? 0
         : state == VideoPowerStandBy ? VCR_DAC_DPMS_ON_HSYNC
         : state == VideoPowerSuspend ? VCR_DAC_DPMS_ON_VSYNC
         : (VCR_DAC_DPMS_ON_VSYNC | VCR_DAC_DPMS_ON_HSYNC);
    d = VcrRd(x, 0, VCR_R_DACMODE) & ~(VCR_DAC_DPMS_ON_VSYNC | VCR_DAC_DPMS_ON_HSYNC);
    VcrWr(x, 0, VCR_R_DACMODE, d | bits);
}
