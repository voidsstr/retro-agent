/*
 * vcrmp_multi.c - the slave chips and SLI/AA, kernel side.
 *
 * The sequence itself is the Glide GPL port in vcrmp_sli.c, which touches the
 * hardware only through a vcr_sli_io accessor table. This file supplies that
 * table over the miniport's own helpers, places and maps the slaves at boot,
 * answers Glide's SLI_AA_REQUEST, and turns SLI off again whenever anything
 * else takes the display - above all a mode set, because a Glide client whose
 * context was lost SKIPS its own teardown (docs/survey-glide-kernel-contract.md)
 * and the next thing that happens is DirectDraw restoring the desktop mode.
 *
 * Accessors:
 *   cfg     VcrPciRead/Write - the HAL for the master, raw 0xCF8 cycles for
 *           the slaves (the HAL cannot see functions 1-3 on this board)
 *   io      VcrRd/VcrWr on each chip's kernel mapping of its memBase0
 *   vga     the SHARED I/O BAR (x->io); which chip answers is whichever has
 *           its command-register I/O decode on, and the sequence toggles that
 *           itself - so `chip` is only the sequence's belief, logged
 *   log     VCR_EV_SLI_STEP, written BEFORE each register write
 *
 * Diag switches (Services\vcrmp\Diag): Sli = 0 keeps Glide single-chip and
 * leaves the slaves untouched; Sli6kClock = 0 skips the external clock.
 */
#include "vcrmp.h"
#include "../include/vcr_sli.h"

#define MB32    0x02000000u

static vcr_u32 k_cfg_rd(void *ctx, vcr_u32 chip, vcr_u32 off)
{
    VCR_EXT *x = (VCR_EXT *)ctx;
    if (chip >= x->nchips)
        return 0xffffffffu;
    return VcrPciRead(x, chip ? x->chip[chip].slot : x->slot, off, 4);
}

static void k_cfg_wr(void *ctx, vcr_u32 chip, vcr_u32 off, vcr_u32 v)
{
    VCR_EXT *x = (VCR_EXT *)ctx;
    if (chip < x->nchips)
        VcrPciWrite(x, chip ? x->chip[chip].slot : x->slot, off, v, 4);
}

static vcr_u32 k_io_rd(void *ctx, vcr_u32 chip, vcr_u32 off)
{
    VCR_EXT *x = (VCR_EXT *)ctx;
    return off < VCR_MMIO_MAP_LEN ? VcrRd(x, chip, off) : 0xffffffffu;
}

static void k_io_wr(void *ctx, vcr_u32 chip, vcr_u32 off, vcr_u32 v)
{
    VCR_EXT *x = (VCR_EXT *)ctx;
    if (off < VCR_MMIO_MAP_LEN)
        VcrWr(x, chip, off, v);
}

static vcr_u8 k_vga_rd(void *ctx, vcr_u32 chip, vcr_u32 port)
{
    (void)chip;
    return VcrVgaRd((VCR_EXT *)ctx, port);
}

static void k_vga_wr(void *ctx, vcr_u32 chip, vcr_u32 port, vcr_u8 v)
{
    (void)chip;
    VcrVgaWr((VCR_EXT *)ctx, port, v);
}

static void k_stall(void *ctx, vcr_u32 us)
{
    (void)ctx;
    VideoPortStallExecution(us);
}

/* The milestones of the sequence also go to the registry, FLUSHED (VcrPhase):
 * a wedge in the middle of an SLI/AA bring-up needs a power cycle, which
 * loses the in-memory flight recorder - Diag\LastPhase / PhaseLog survive and
 * say which step the box never got past (a = step, b = chip << 24 | register). */
static int vcr_sli_step_persists(vcr_u32 step)
{
    return step % 100 == 0 || step == VCR_SLI_S_MAP_DONE || step == VCR_SLI_S_PCIINIT0 ||
           step == VCR_SLI_S_CLOCK_6K || step == VCR_SLI_S_SLICTRL ||
           step == VCR_SLI_S_SET_DONE || step == VCR_SLI_S_OFF_DONE || step >= 900;
}

static void k_log(void *ctx, vcr_u32 step, vcr_u32 chip, vcr_u32 reg, vcr_u32 val,
                  const char *what)
{
    int keep = vcr_sli_step_persists(step);
    ULONG lv = step >= 900 ? VCR_LV_WARN : keep ? VCR_LV_INFO : VCR_LV_DEBUG;
    (void)ctx;
    VLOG(lv, VCR_EV_SLI_STEP, step, chip, reg, val, "%s", what);
    if (keep)
        VcrPhase(VCR_EV_SLI_STEP, step, (chip << 24) | (reg & 0xffffff), what);
}

static void make_io(VCR_EXT *x, vcr_sli_io *io)
{
    io->ctx = x;
    io->cfg_rd = k_cfg_rd;
    io->cfg_wr = k_cfg_wr;
    io->io_rd = k_io_rd;
    io->io_wr = k_io_wr;
    io->vga_rd = k_vga_rd;
    io->vga_wr = k_vga_wr;
    io->stall_us = k_stall;
    io->log = k_log;
}

/* The hook vcrmp_sli.c calls for a 4-chip board (built with
 * VCR_SLI_HAVE_6K_CLOCK): the external clock follows the master's pixel
 * clock, which the mode set before SLI_AA_REQUEST has just programmed. */
int vcr_sli_6k_clock(const vcr_sli_io *io)
{
    VCR_EXT *x = (VCR_EXT *)io->ctx;
    ULONG hz;
    if (!VcrDiagGet(L"Sli6kClock", 1)) {
        VLOG(VCR_LV_WARN, VCR_EV_CLOCK_6K, 0, 0, 0, 0, "external clock skipped (Diag\\Sli6kClock=0)");
        return VCR_SLI_ENOTIMPL;
    }
    hz = VcrClock6k(x, VcrRd(x, 0, VCR_R_PLLCTRL0));
    if (hz)
        x->clock_6k_hz = hz;
    return hz ? VCR_SLI_OK : VCR_SLI_ENOTIMPL;
}

/* At FindAdapter: give the slaves their BARs (mapSlavePhysical) and map each
 * one's registers. Only then does Glide hear about more than one chip. */
void VcrMultiInit(VCR_EXT *x)
{
    vcr_sli_io io;
    vcr_u32 b0[VCR_MAX_CHIPS] = { 0 }, b1[VCR_MAX_CHIPS] = { 0 };
    ULONG c, n = x->nchips;
    int rc;

    x->glide_chips = 1;
    if (x->backend != VCR_HW_VOODOO || !VCR_IS_NAPALM(x->device) || n < 2)
        return;
    if (!VcrDiagGet(L"Sli", 1)) {
        VLOG(VCR_LV_INFO, VCR_EV_SLI_DONE, 0, n, 0, 0,
             "%u chips, multi-chip off (Diag\\Sli=0): Glide runs on the master", n);
        return;
    }
    if (n != 2 && n != 4) {
        VLOG(VCR_LV_WARN, VCR_EV_SLI_DONE, 0, n, 0, 0, "%u chips is not a VSA-100 board", n);
        return;
    }
    /* The slaves go INSIDE the master's own windows (master + 32 MB * chip,
     * and master LFB + 64 MB), which PnP sized from the power-up decode (128 MB
     * and 256 MB on the 6000). Outside our resources the bridge may not route
     * them at all - refuse rather than guess. */
    if (x->mmio_len < n * MB32 || x->lfb_len < 4 * MB32) {
        VLOG(VCR_LV_WARN, VCR_EV_SLI_DONE, 0, n, x->mmio_len, x->lfb_len,
             "BAR windows too small for %u chips - Glide stays single-chip", n);
        return;
    }
    make_io(x, &io);
    VcrPhase(VCR_EV_SLI_STEP, VCR_SLI_S_MAP_BEGIN, n, "placing the slave chips");
    rc = vcr_sli_map_slaves(&io, n, b0, b1);
    if (rc < 0 || (rc & VCR_SLI_W_READBACK)) {
        VLOG(VCR_LV_ERROR, VCR_EV_SLI_DONE, 0, n, (ULONG)rc, 0,
             "slave placement failed (%d) - Glide stays single-chip", rc);
        return;
    }
    for (c = 1; c < n; c++) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = b0[c];
        if (b0[c] < x->chip[0].mmio_phys.LowPart ||
            b0[c] + VCR_MMIO_MAP_LEN > x->chip[0].mmio_phys.LowPart + x->mmio_len) {
            VLOG(VCR_LV_ERROR, VCR_EV_SLI_DONE, 0, c, b0[c], x->mmio_len,
                 "slave %u at %08x is outside the master's window", c, b0[c]);
            return;
        }
        if (x->chip[c].regs && x->chip[c].mmio_phys.QuadPart != pa.QuadPart) {
            VideoPortFreeDeviceBase(x, x->chip[c].regs);
            x->chip[c].regs = NULL;
        }
        x->chip[c].mmio_phys = pa;
        x->chip[c].lfb_phys.QuadPart = b1[c];
        x->chip[c].io_phys = x->chip[0].io_phys;
        if (!x->chip[c].regs)
            x->chip[c].regs = (PUCHAR)VideoPortGetDeviceBase(x, pa, VCR_MMIO_MAP_LEN, FALSE);
        VLOG(x->chip[c].regs ? VCR_LV_INFO : VCR_LV_ERROR, VCR_EV_MAP, 0x10 + c, b0[c],
             VCR_MMIO_MAP_LEN, (ULONG)(ULONG_PTR)x->chip[c].regs,
             "slave %u registers mapped, status %08x", c,
             x->chip[c].regs ? VcrRd(x, c, VCR_R_STATUS) : 0xffffffffu);
        if (!x->chip[c].regs)
            return;
    }
    x->glide_chips = n;
    VcrPhase(VCR_EV_SLI_DONE, 0, n, "slaves placed");
    VLOG(VCR_LV_INFO, VCR_EV_SLI_DONE, 0, n, (ULONG)rc, 0,
         "%u chips placed and mapped - Glide may use SLI", n);
}

static int sli_disable(VCR_EXT *x, ULONG n)
{
    vcr_sli_io io;
    vcr_sli_aa_req r;
    VideoPortZeroMemory(&r, sizeof r);
    r.ChipInfo.dwChips = n;
    make_io(x, &io);
    return vcr_sli_set(&io, &r);
}

void VcrSliOff(VCR_EXT *x, const char *why)
{
    ULONG n = x->sli_chips;
    int rc;
    if (!n)
        return;
    VLOG(VCR_LV_INFO, VCR_EV_SLI_DONE, 0, n, 0, 0, "SLI/AA off: %s", why);
    rc = sli_disable(x, n);
    x->sli_result = rc;
    x->sli_chips = 0;
    x->sli_active = 0;
    VLOG(rc ? VCR_LV_WARN : VCR_LV_INFO, VCR_EV_SLI_DONE, 0, n, (ULONG)rc, 0,
         "SLI/AA off -> %d", rc);
}

VP_STATUS VcrSliRequest(VCR_EXT *x, const void *req, ULONG len, vcr_sli_res *out)
{
    vcr_sli_aa_req rq;
    const vcr_sli_aa_req *r = &rq;
    vcr_sli_io io;
    ULONG n, en;
    int rc;

    /* METHOD_BUFFERED: `req` and `out` are the SAME system buffer. Take the
     * request before anything is written to the answer - zeroing `out` first
     * wiped dwChips/sliEn/aaEn and turned Glide's enable into a disable
     * (the first 4-chip run on .124, 2026-09-26). */
    if (len < sizeof rq)
        return ERROR_INSUFFICIENT_BUFFER;
    VideoPortMoveMemory(&rq, (PVOID)req, sizeof rq);
    VideoPortZeroMemory(out, sizeof *out);
    n = r->ChipInfo.dwChips;
    en = r->ChipInfo.dwsliEn || r->ChipInfo.dwaaEn;
    VcrPhase(VCR_EV_HWC_SLIAA, n, (r->ChipInfo.dwsliEn ? 1 : 0) | (r->ChipInfo.dwaaEn ? 2 : 0) |
             (r->ChipInfo.dwaaSampleHigh << 4) | (r->ChipInfo.dwsliAaAnalog << 8),
             en ? "SLI_AA_REQUEST enable" : "SLI_AA_REQUEST disable");
    VLOG(VCR_LV_INFO, VCR_EV_HWC_SLIAA, n, r->ChipInfo.dwsliEn, r->ChipInfo.dwaaEn,
         r->ChipInfo.dwsli_nlines, "SLI_AA_REQUEST: %u chips, analog %u, sample %u, bpp %u",
         n, r->ChipInfo.dwsliAaAnalog, r->ChipInfo.dwaaSampleHigh, r->MemInfo.dwBpp);

    if (!en) {
        /* A disable carries garbage in everything but dwChips. Undo what WE
         * enabled; with nothing enabled there is nothing to undo. */
        if (x->sli_chips)
            VcrSliOff(x, "Glide asked");
        rc = x->sli_result = 0;
    } else if (x->backend != VCR_HW_VOODOO || n < 1 || n > x->glide_chips) {
        rc = VCR_SLI_EINVAL;
        VLOG(VCR_LV_WARN, VCR_EV_SLI_DONE, 1, n, (ULONG)rc, x->glide_chips,
             "refused: %u chips asked, %u available", n, x->glide_chips);
    } else {
        make_io(x, &io);
        if (x->sli_chips)
            VcrSliOff(x, "re-enable");
        rc = vcr_sli_set(&io, r);
        if (rc >= 0) {
            x->sli_chips = n;
            x->sli_active = 1;
        }
        VLOG(rc ? VCR_LV_WARN : VCR_LV_INFO, VCR_EV_SLI_DONE, 1, n, (ULONG)rc, x->clock_6k_hz,
             "SLI/AA on: %u chips -> %d, clock %u Hz", n, rc, x->clock_6k_hz);
    }
    x->sli_result = rc;
    out->result = (vcr_u32)rc;
    out->sli_chips = x->sli_chips;
    out->clock_6k_hz = x->clock_6k_hz;
    return NO_ERROR;
}
