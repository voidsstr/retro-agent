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
 * leaves the slaves untouched; Sli6kClock = 0 skips the external clock;
 * SliAA = 1 lets an AA request through (absent/0: refused before any write -
 * the AA kill switch, read per request); SliPersistAll = 1 makes every SLI
 * step a flushed phase (supervised runs; slow). AllowPoke = 1 (vcrmp.c) is the
 * only way a PCI_OP may write the SLI/AA config registers.
 * SliAAVendorRecipe = 1 runs an AA request with the vendor-style recipe
 * (vcr_sli_set_ex, VCR_SLI_F_VENDOR_AA; absent/0 = the dos_mode.c-derived
 * sequence), read per request - for supervised A/B runs only.
 * SliAAReadback = 1: after an AA enable, every chip's SLI/AA config space is
 * read back by config cycles into SliAAState (REG_BINARY, flushed;
 * vcr_sli_aa_readback; tools/vcrphases.py decodes it). Absent/0 = no
 * read-back: the AA path keeps the timing it had on 2026-09-26.
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
 * say which step the box never got past. a = step; b = vcr_sli_phase_b()
 * (chip << 24 | register, or - for SET_DONE / OFF_DONE / CLOCK_6K / NOMUX /
 * REFUSED - the value that step exists to report: the warn mask, the clock
 * result, the refusal reason and request shape). Which steps: the milestones
 * (vcr_sli_step_persists), or every step while Diag\SliPersistAll = 1
 * (x->sli_persist_all, read at FindAdapter and at every request). */
static void k_log(void *ctx, vcr_u32 step, vcr_u32 chip, vcr_u32 reg, vcr_u32 val,
                  const char *what)
{
    VCR_EXT *x = (VCR_EXT *)ctx;
    int keep = vcr_sli_step_persists(step, x ? x->sli_persist_all : 0);
    ULONG lv = step >= 900 ? VCR_LV_WARN : vcr_sli_step_persists(step, 0) ? VCR_LV_INFO
                                                                          : VCR_LV_DEBUG;
    /* pciInit0 has no config-space alias, so Diag\SliAAState records the value
     * the sequence writes - taken here, where the sequence announces it */
    if (x && step == VCR_SLI_S_PCIINIT0 && chip < VCR_SLI_MAX_CHIPS) {
        x->sli_pci0[chip] = val;
        x->sli_pci0_mask |= 1u << chip;
    }
    VLOG(lv, VCR_EV_SLI_STEP, step, chip, reg, val, "%s", what);
    if (keep)
        VcrPhase(VCR_EV_SLI_STEP, step, vcr_sli_phase_b(step, chip, reg, val), what);
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
    io->fb_bytes = x->fb_per_chip;      /* memBase1 = 2 x this (the 256 MB VBIOS mode: 64 MB) */
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
    /* before the first step is logged: the slave placement below is steps too */
    x->sli_persist_all = VcrDiagGet(L"SliPersistAll", 0);
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
     * and master LFB + 2 x memory, shared by every slave), which PnP sized
     * from the power-up decode (128 MB and 256 MB on the 6000). The LFB
     * window must hold the master's 2 x memory and the slaves' 2 x memory:
     * 128 MB at 32 MB/chip, 256 MB at 64 MB/chip (the 256 MB VBIOS mode).
     * Outside our resources the bridge may not route them at all - refuse
     * rather than guess. */
    if (x->mmio_len < n * MB32 || x->lfb_len < 4 * (x->fb_per_chip ? x->fb_per_chip : MB32)) {
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

/* THE AA KILL SWITCH. Every AA configuration tried on .124 froze the whole PC
 * (cfg 3 in an LFB read, cfg 7 and cfg 1 inside Glide's open; 2026-09-26) and
 * a frozen box needs a person at the power switch. So an AA request is refused
 * - before a single register is written, before a live SLI session is torn
 * down to make room - unless Services\vcrmp\Diag\SliAA (DWORD) is 1. Absent =
 * 0. Read at every request, so a supervised session can arm it for one run and
 * disarm it without a reboot. SLI-only requests and every disable are
 * unaffected (vcr_sli_policy). */
static int sli_aa_allowed(void)
{
    return VcrDiagGet(L"SliAA", 0) != 0;
}

/* The same switch for pci_op (vcrmp.c): Glide's single-chip AA path programs
 * AA through PCI_OP writes, not an SLI_AA_REQUEST - the kill switch covers
 * those too (vcr_sli_poke_policy VCR_POKE_R_AA_OFF). */
ULONG VcrSliAAAllowed(void)
{
    return sli_aa_allowed() ? 1 : 0;
}

/* Diag\SliAAVendorRecipe (DWORD, absent = 0): the vendor-style AA recipe for
 * THIS request (vcr_sli.h vcr_sli_set_ex). Read per request, like SliAA, so a
 * supervised run A/Bs the two recipes one clean boot each without a rebuild.
 * It changes nothing but AA requests, and those are refused unless SliAA = 1. */
static vcr_u32 sli_recipe(void)
{
    return VcrDiagGet(L"SliAAVendorRecipe", 0) ? VCR_SLI_F_VENDOR_AA : 0;
}

/* Diag\SliAAReadback (DWORD, absent = 0): after an AA enable, read every
 * chip's SLI/AA config space back into Diag\SliAAState. OFF by default: it
 * adds 9 config reads per chip, ~5 flushed phases and a flushed REG_BINARY
 * write - 100-200 ms - between SET_DONE and Glide's next MMIO, so an AA run
 * without it keeps the timing and traffic of the kernel that ran on
 * 2026-09-26. Read per request, like SliAA. (Not "SliAAState": that name is
 * the REG_BINARY record, and a DWORD switch under it would be overwritten.) */
static int sli_aa_readback_wanted(void)
{
    return VcrDiagGet(L"SliAAReadback", 0) != 0;
}

/* After an AA enable: every chip's SLI/AA config space, read back by config
 * cycles only, into Diag\SliAAState (REG_BINARY, flushed) - so a supervised
 * run that wedges later still says what the kernel had programmed. */
static void sli_aa_state(VCR_EXT *x, const vcr_sli_io *io, const vcr_sli_aa_req *r, int rc,
                         vcr_u32 recipe)
{
    vcr_sli_aa_state st;
    int n = vcr_sli_aa_readback(io, r, rc, recipe, x->sli_pci0, x->sli_pci0_mask, &st);
    if (n <= 0)
        return;
    st.boot = VcrDiagGet(L"BootCount", 0);
    st.ms = VcrMs();
    VcrDiagSetBinary(L"SliAAState", &st, sizeof st, TRUE);
    VLOG(VCR_LV_INFO, VCR_EV_SLI_DONE, 2, (ULONG)n, st.chip[0].cfg[7], st.flags,
         "SLI/AA state: %d chips read back (config cycles) into Diag\\SliAAState, "
         "chip 0 aaLfbCtrl %08x", n, st.chip[0].cfg[7]);
}

VP_STATUS VcrSliRequest(VCR_EXT *x, const void *req, ULONG len, vcr_sli_res *out)
{
    vcr_sli_aa_req rq;
    const vcr_sli_aa_req *r = &rq;
    vcr_sli_io io;
    ULONG n, en;
    vcr_u32 recipe;
    int rc, why;

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
    x->sli_persist_all = VcrDiagGet(L"SliPersistAll", 0);
    /* the vendor AA recipe changes AA requests only - read for those alone */
    recipe = (en && r->ChipInfo.dwaaEn) ? sli_recipe() : 0;

    /* The policy comes first, before anything below can write: a refused
     * request leaves the board - and any live session - exactly as it was.
     * That includes the vendor recipe's memory check (policy_ex): it used to
     * run only inside the sequence, after the live session was torn down.
     * The refusal is a persisted phase (k_log: REFUSED is a 9xx step), with
     * the reason and the request's shape in b (vcr_sli_phase_b). */
    why = en ? vcr_sli_policy_ex(r, sli_aa_allowed(), recipe) : 0;
    if (why) {
        k_log(x, VCR_SLI_S_REFUSED, 0, (vcr_u32)why,
              why == VCR_SLI_R_MEMINFO ? r->MemInfo.dwTileMark >> 12 : vcr_sli_req_tuple(r),
              why == VCR_SLI_R_AA_OFF
                  ? "AA request refused before any write: Diag\\SliAA is 0 (the AA kill switch)"
              : why == VCR_SLI_R_MEMINFO
                  ? "vendor AA recipe: tileMark/totalMemory unusable: refused before any write"
                  : "no video-mux branch for this chip/SLI/AA combination: refused before any write");
        rc = why == VCR_SLI_R_AA_OFF ? VCR_SLI_EDENIED : VCR_SLI_EINVAL;
        VLOG(VCR_LV_WARN, VCR_EV_SLI_DONE, 1, n, (ULONG)rc, vcr_sli_req_tuple(r),
             "SLI/AA request refused (%d), reason %u, shape %05x - nothing written, %u chips live",
             rc, why, vcr_sli_req_tuple(r), x->sli_chips);
    } else if (!en) {
        /* A disable carries garbage in everything but dwChips. Undo what WE
         * enabled; with nothing enabled there is nothing to undo. */
        if (x->sli_chips)
            VcrSliOff(x, "Glide asked");
        rc = x->sli_result = 0;
    } else if (x->backend != VCR_HW_VOODOO || !VCR_IS_NAPALM(x->device) || n < 1 ||
               n > x->glide_chips) {
        /* VCR_HW_VOODOO is Banshee / Voodoo 3 as well: only a VSA-100 has
         * SLI/AA registers (vcr_sli_set_ex asks chip 0 again, as a backstop) */
        rc = VCR_SLI_EINVAL;
        VLOG(VCR_LV_WARN, VCR_EV_SLI_DONE, 1, n, (ULONG)rc, x->glide_chips,
             "refused: %u chips asked, %u available, device %04x", n, x->glide_chips, x->device);
    } else {
        make_io(x, &io);
        if (x->sli_chips)
            VcrSliOff(x, "re-enable");
        x->sli_pci0_mask = 0;           /* only THIS enable's pciInit0 writes count */
        rc = vcr_sli_set_ex(&io, r, recipe);
        if (rc >= 0) {
            x->sli_chips = n;
            x->sli_active = 1;
        }
        VLOG(rc ? VCR_LV_WARN : VCR_LV_INFO, VCR_EV_SLI_DONE, 1, n, (ULONG)rc, x->clock_6k_hz,
             "SLI/AA on: %u chips -> %d, clock %u Hz%s", n, rc, x->clock_6k_hz,
             recipe ? ", vendor AA recipe" : "");
        /* after SET_DONE, AA only, Diag\SliAAReadback only: config cycles,
         * nothing through a BAR */
        if (vcr_sli_aa_state_wanted(r, rc) && sli_aa_readback_wanted())
            sli_aa_state(x, &io, r, rc, recipe);
    }
    x->sli_result = rc;
    out->result = (vcr_u32)rc;
    out->sli_chips = x->sli_chips;
    out->clock_6k_hz = x->clock_6k_hz;
    return NO_ERROR;
}
