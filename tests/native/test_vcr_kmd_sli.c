/* test_vcr_kmd_sli.c
 *
 * The VSA-100 multi-chip (SLI / AA) bring-up of the vcr-kmd miniport
 * (voodoo-cleanroom/vcr-kmd/miniport/vcrmp_sli.c), a port of the Glide GPL
 * dos_mode.c sequence (mapSlavePhysical / initSlave / setVideoModeSlave /
 * hwcSetSLIAAMode), run against a MOCK of four VSA-100s seeded with what was
 * measured on the V5 6000 in .124 (master BAR0 0xd0000000, BAR1 0xc0000000,
 * I/O 0xc000, decode 0x45; slaves command 0x0002, cfgInitEnable 0x301,
 * cfgPciDecode 0x00011445).
 *
 * The mock models what makes this sequence dangerous on the real board:
 *   - ONE shared I/O BAR: a VGA access reaches whichever chip has Command
 *     bit 0 set. An access while 0 or 2+ chips decode, or while the sequence
 *     believes it is talking to a different chip, is counted.
 *   - read-only fields (fab ID, BAR type bits) so read-backs are realistic;
 *   - memBase0 snooping of the master's 3D registers, so the sliCtrl order
 *     (master first) is actually tested;
 *   - a status register that can be stuck with no FIFO room, forever.
 * Every write must be announced by the log entry right before it.
 */
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/miniport/vcrmp_sli.c"

#define NCH     4
#define MAXW    8192
#define MAXL    8192

typedef struct { char kind; vcr_u32 chip, off, val; } wrec;
typedef struct { vcr_u32 step, chip, reg, val; } lrec;

typedef struct mock {
    int present[NCH];
    vcr_u32 cfg[NCH][64];
    vcr_u32 io[NCH][64];
    vcr_u32 slictrl[NCH];
    unsigned slictrl_direct[NCH];       /* direct writes (not snooped copies) */
    /* the VGA register file of each chip */
    vcr_u8 crtc[NCH][0x40], seq[NCH][8], attr[NCH][0x20], misc[NCH];
    vcr_u8 crtc_i[NCH], seq_i[NCH], attr_i[NCH], attr_ff[NCH], wake[NCH], fc[NCH];
    int status_stuck[NCH];
    unsigned long status_reads[NCH], stall_calls, stall_us;
    wrec w[MAXW]; unsigned nw;
    lrec l[MAXL]; unsigned nl;
    /* the pending log entry, and the checks */
    int pend_valid, pend_stage;
    vcr_u32 pend_chip, pend_reg, pend_val;
    unsigned unlogged, vga_wrong_chip, vga_no_decoder, io_decode_overlap, slave_unmapped;
} mock;

static mock M;

#define CFGI(off)   ((off) >> 2)

/* ---- the mock ------------------------------------------------------------------ */

static int decoders(mock *m, int *which)
{
    int c, n = 0;
    for (c = 0; c < NCH; c++)
        if (m->present[c] && (m->cfg[c][CFGI(0x04)] & 1)) {
            n++;
            *which = c;
        }
    return n;
}

static void take_log(mock *m, vcr_u32 chip, vcr_u32 reg, vcr_u32 val)
{
    if (m->pend_valid && m->pend_chip == chip && m->pend_reg == reg && m->pend_val == val)
        m->pend_valid = 0;
    else
        m->unlogged++;
}

static void trace(mock *m, char kind, vcr_u32 chip, vcr_u32 off, vcr_u32 val)
{
    if (m->nw < MAXW) {
        m->w[m->nw].kind = kind;
        m->w[m->nw].chip = chip;
        m->w[m->nw].off = off;
        m->w[m->nw].val = val;
        m->nw++;
    }
}

static void m_log(void *ctx, vcr_u32 step, vcr_u32 chip, vcr_u32 reg, vcr_u32 val,
                  const char *what)
{
    mock *m = ctx;
    if (m->nl < MAXL) {
        m->l[m->nl].step = step;
        m->l[m->nl].chip = chip;
        m->l[m->nl].reg = reg;
        m->l[m->nl].val = val;
        m->nl++;
    }
    m->pend_valid = 1;
    m->pend_stage = 0;
    m->pend_chip = chip;
    m->pend_reg = reg;
    m->pend_val = val;
    if (!what)
        m->unlogged += 1000;            /* every entry must say what it is */
}

static vcr_u32 m_cfg_rd(void *ctx, vcr_u32 chip, vcr_u32 off)
{
    mock *m = ctx;
    if (chip >= NCH || !m->present[chip])
        return 0xffffffffu;             /* nobody answers the config cycle */
    return m->cfg[chip][CFGI(off & 0xfc)];
}

static void m_cfg_wr(void *ctx, vcr_u32 chip, vcr_u32 off, vcr_u32 v)
{
    mock *m = ctx;
    vcr_u32 *r;
    int who;
    take_log(m, chip, off, v);
    trace(m, 'c', chip, off, v);
    if (chip >= NCH || !m->present[chip])
        return;
    r = &m->cfg[chip][CFGI(off & 0xfc)];
    switch (off) {
    case 0x00: break;                                       /* IDs: read-only */
    case 0x04: *r = (*r & 0xffff0000u) | (v & 0xffff); break;   /* status: RO here */
    case 0x10: *r = v & ~0xfu; break;                       /* memory, 32-bit */
    case 0x14: *r = (v & ~0xfu) | 0x8; break;               /* prefetchable */
    case 0x18: *r = (v & ~0x3u) | 0x1; break;               /* I/O space */
    case 0x40: *r = (v & ~0xffu) | (*r & 0xff); break;      /* fab ID: RO */
    default:   *r = v; break;
    }
    if (off == 0x04 && decoders(m, &who) > 1)
        m->io_decode_overlap++;
}

static int slave_mapped(mock *m, vcr_u32 chip)
{
    return chip == 0 || (m->cfg[chip][CFGI(0x10)] && (m->cfg[chip][CFGI(0x04)] & 2));
}

static vcr_u32 m_io_rd(void *ctx, vcr_u32 chip, vcr_u32 off)
{
    mock *m = ctx;
    if (chip >= NCH || !m->present[chip])
        return 0xffffffffu;
    if (!slave_mapped(m, chip))
        m->slave_unmapped++;
    if (off == VCR_R_STATUS) {
        m->status_reads[chip]++;
        /* stuck: no FIFO room, and it never changes (vretrace bit only) */
        return m->status_stuck[chip] ? 0x40u : m->io[chip][0];
    }
    if (off < 0x100)
        return m->io[chip][off >> 2];
    if (off == VCR_3D_SLICTRL)
        return m->slictrl[chip];
    return 0;
}

static void m_io_wr(void *ctx, vcr_u32 chip, vcr_u32 off, vcr_u32 v)
{
    mock *m = ctx;
    int s;
    take_log(m, chip, off, v);
    trace(m, 'i', chip, off, v);
    if (chip >= NCH || !m->present[chip])
        return;
    if (!slave_mapped(m, chip))
        m->slave_unmapped++;
    if (off < 0x100) {
        if (off != VCR_R_STATUS)
            m->io[chip][off >> 2] = v;
        return;
    }
    if (off == VCR_3D_SLICTRL) {
        m->slictrl[chip] = v;
        m->slictrl_direct[chip]++;
        /* a write to the master's memBase0 is snooped by every slave whose
         * cfgInitEnable says so (memBase0 snoop + snoop slave) */
        if (chip == 0)
            for (s = 1; s < NCH; s++) {
                vcr_u32 ie = m->cfg[s][CFGI(0x40)] >> 8;
                if (m->present[s] && (ie & VCR_IE_MEMBASE0_SNOOP_EN) &&
                    (ie & VCR_IE_ADDRESS_SNOOP_SLAVE))
                    m->slictrl[s] = v;
            }
    }
}

/* the shared I/O BAR: the access goes to the chip that decodes it */
static int vga_target(mock *m, vcr_u32 chip)
{
    int who = -1;
    if (decoders(m, &who) != 1) {
        m->vga_no_decoder++;
        return -1;
    }
    if ((vcr_u32)who != chip)
        m->vga_wrong_chip++;
    return who;
}

static vcr_u8 m_vga_rd(void *ctx, vcr_u32 chip, vcr_u32 port)
{
    mock *m = ctx;
    int d = vga_target(m, chip);
    if (d < 0)
        return 0xff;
    switch (port) {
    case 0x3d5: return m->crtc[d][m->crtc_i[d] & 0x3f];
    case 0x3c5: return m->seq[d][m->seq_i[d] & 7];
    case 0x3cc: return m->misc[d];
    case 0x3da: m->attr_ff[d] = 0; return 0;
    }
    return 0xff;
}

static void m_vga_wr(void *ctx, vcr_u32 chip, vcr_u32 port, vcr_u8 v)
{
    mock *m = ctx;
    int d;

    /* logged? a plain write matches (port, v); an indexed pair matches the
     * index write against VCR_SLI_VGA_IDX(port, v) and the data write
     * against (port - 1, v). */
    if (m->pend_valid && m->pend_chip == chip && !(m->pend_reg & VCR_SLI_VGA_INDEXED) &&
        m->pend_reg == port && m->pend_val == v) {
        m->pend_valid = 0;
    } else if (m->pend_valid && m->pend_chip == chip && m->pend_stage == 0 &&
               m->pend_reg == VCR_SLI_VGA_IDX(port, v)) {
        m->pend_stage = 1;
    } else if (m->pend_valid && m->pend_chip == chip && m->pend_stage == 1 &&
               (m->pend_reg & 0xffff) + 1 == port && m->pend_val == v) {
        m->pend_valid = 0;
    } else {
        m->unlogged++;
    }
    trace(m, 'v', chip, port, v);

    d = vga_target(m, chip);
    if (d < 0)
        return;
    switch (port) {
    case 0x3d4: m->crtc_i[d] = v; break;
    case 0x3d5: m->crtc[d][m->crtc_i[d] & 0x3f] = v; break;
    case 0x3c4: m->seq_i[d] = v; break;
    case 0x3c5: m->seq[d][m->seq_i[d] & 7] = v; break;
    case 0x3c2: m->misc[d] = v; break;
    case 0x3c3: m->wake[d] = v; break;
    case 0x3da: m->fc[d] = v; break;
    case 0x3c0:
        if (!m->attr_ff[d])
            m->attr_i[d] = v;
        else
            m->attr[d][m->attr_i[d] & 0x1f] = v;
        m->attr_ff[d] ^= 1;
        break;
    }
}

static void m_stall(void *ctx, vcr_u32 us)
{
    mock *m = ctx;
    m->stall_calls++;
    m->stall_us += us;
}

/* master's IO-register seeds */
#define M_PCIINIT0      0x0403e003u     /* retry interval + force-FB-high set */
#define M_MISCINIT0     0x40012000u     /* raw-LFB byte swizzle (bit 30) set */
#define M_MISCINIT1     0x00000000u
#define M_TMUGBEINIT    0x01e00f00u     /* AA clock delay 0xf */
#define M_VIDPROCCFG    0x00040c81u     /* RGB565 desktop, video processor on */
#define M_PLLCTRL0      0x0000b855u
#define M_SCREENSIZE    0x001e0280u

static void seed(mock *m, int nchips)
{
    int c, i;
    memset(m, 0, sizeof *m);
    for (c = 0; c < nchips; c++) {
        m->present[c] = 1;
        m->cfg[c][CFGI(0x00)] = 0x0009121au;            /* 121a:0009 VSA-100 */
        m->cfg[c][CFGI(0x04)] = 0x02300002u;            /* memory on, I/O off */
        m->cfg[c][CFGI(0x40)] = 0x00000301u;            /* measured */
        m->cfg[c][CFGI(0x48)] = 0x00011445u;            /* measured (slaves) */
        m->cfg[c][CFGI(0xac)] = 0x00000800u;            /* cfgSliAAMisc power-up (measured) */
        m->io[c][0] = 0x0000005fu;                      /* status: idle, FIFO free */
        m->io[c][VCR_R_VIDPROCCFG >> 2] = 0x04000011u;  /* 2X + half mode, off */
    }
    /* the master: function 0, as PnP left it after the miniport's narrowing */
    m->cfg[0][CFGI(0x04)] = 0x02300003u;                /* I/O and memory on */
    m->cfg[0][CFGI(0x10)] = 0xd0000000u;
    m->cfg[0][CFGI(0x14)] = 0xc0000008u;
    m->cfg[0][CFGI(0x18)] = 0x0000c001u;
    m->cfg[0][CFGI(0x48)] = 0x00000045u;
    m->io[0][VCR_R_PCIINIT0 >> 2] = M_PCIINIT0;
    m->io[0][VCR_R_MISCINIT0 >> 2] = M_MISCINIT0;
    m->io[0][VCR_R_MISCINIT1 >> 2] = M_MISCINIT1;
    m->io[0][VCR_R_DRAMINIT0 >> 2] = 0x0c17a9e9u;
    m->io[0][VCR_R_DRAMINIT1 >> 2] = 0x00f02200u;
    m->io[0][VCR_R_TMUGBEINIT >> 2] = M_TMUGBEINIT;
    m->io[0][VCR_R_PLLCTRL1 >> 2] = 0x00003c04u;
    m->io[0][VCR_R_PLLCTRL0 >> 2] = M_PLLCTRL0;
    m->io[0][VCR_R_DACMODE >> 2] = 0;
    m->io[0][VCR_R_VIDPROCCFG >> 2] = M_VIDPROCCFG;
    m->io[0][VCR_R_VIDSCREENSIZE >> 2] = M_SCREENSIZE;
    m->io[0][VCR_R_VIDDESKTOPOVERLAYSTRIDE >> 2] = 0x00000500u;
    m->io[0][VCR_R_VIDOVERLAYSTARTCOORDS >> 2] = 0x11111111u;
    m->io[0][VCR_R_VIDOVERLAYENDSCREENCOORD >> 2] = 0x0077f27fu;
    m->io[0][VCR_R_VIDOVERLAYDUDXOFFSETSRCWIDTH >> 2] = 0x05000000u;
    m->io[0][VCR_R_VIDOVERLAYDUDX >> 2] = 0x00100000u;
    m->io[0][VCR_R_VIDMAXRGBDELTA >> 2] = 0x00100810u;
    /* the master's VGA: a mode, every captured byte distinct */
    for (i = 0; i < 0x40; i++)
        m->crtc[0][i] = (vcr_u8)(0x40 + i);
    m->misc[0] = 0x2f;
    m->seq[0][1] = 0x21;                                /* bit 5 (screen off) set */
}

static vcr_sli_io mkio(mock *m)
{
    vcr_sli_io io;
    memset(&io, 0, sizeof io);
    io.ctx = m;
    io.cfg_rd = m_cfg_rd;
    io.cfg_wr = m_cfg_wr;
    io.io_rd = m_io_rd;
    io.io_wr = m_io_wr;
    io.vga_rd = m_vga_rd;
    io.vga_wr = m_vga_wr;
    io.stall_us = m_stall;
    io.log = m_log;
    return io;
}

static vcr_u32 CFG(mock *m, int c, vcr_u32 off) { return m->cfg[c][CFGI(off)]; }
static vcr_u32 IOR(mock *m, int c, vcr_u32 off) { return m->io[c][off >> 2]; }

static unsigned count_w(mock *m, char kind, vcr_u32 chip, vcr_u32 off)
{
    unsigned i, n = 0;
    for (i = 0; i < m->nw; i++)
        if (m->w[i].kind == kind && m->w[i].chip == chip && m->w[i].off == off)
            n++;
    return n;
}

static int has_step(mock *m, vcr_u32 step)
{
    unsigned i;
    for (i = 0; i < m->nl; i++)
        if (m->l[i].step == step)
            return 1;
    return 0;
}

static vcr_sli_aa_req req(vcr_u32 chips, vcr_u32 sli, vcr_u32 aa, vcr_u32 high,
                          vcr_u32 analog, vcr_u32 nlines, vcr_u32 bpp)
{
    vcr_sli_aa_req r;
    memset(&r, 0, sizeof r);
    r.ChipInfo.dwChips = chips;
    r.ChipInfo.dwsliEn = sli;
    r.ChipInfo.dwaaEn = aa;
    r.ChipInfo.dwaaSampleHigh = high;
    r.ChipInfo.dwsliAaAnalog = analog;
    r.ChipInfo.dwsli_nlines = nlines;
    r.ChipInfo.dwCfgSwapAlgorithm = 1;          /* what Glide always sends */
    r.MemInfo.dwTotalMemory = 32u << 20;
    r.MemInfo.dwBpp = bpp;
    return r;
}

/* map the slaves the way the kernel will before any request */
static void mapped(mock *m, vcr_sli_io *io, int nchips)
{
    vcr_u32 b0[4], b1[4];
    seed(m, nchips);
    *io = mkio(m);
    CHECK(vcr_sli_map_slaves(io, (vcr_u32)nchips, b0, b1) >= 0, "map failed");
}

static void no_bus_faults(mock *m)
{
    CHECK_EQ_U(m->unlogged, 0);
    CHECK_EQ_U(m->vga_wrong_chip, 0);
    CHECK_EQ_U(m->vga_no_decoder, 0);
    CHECK_EQ_U(m->io_decode_overlap, 0);
    CHECK_EQ_U(m->slave_unmapped, 0);
}

/* ---- mapSlavePhysical ------------------------------------------------------------ */

TEST(map_puts_each_slave_32mb_above_the_last_and_all_bar1s_above_the_master) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_u32 b0[4], b1[4];
    int c;
    seed(m, 4);
    io = mkio(m);
    CHECK_EQ_I(vcr_sli_map_slaves(&io, 4, b0, b1), VCR_SLI_OK);
    CHECK_EQ_U(b0[0], 0xd0000000u);
    CHECK_EQ_U(b0[1], 0xd2000000u);
    CHECK_EQ_U(b0[2], 0xd4000000u);
    CHECK_EQ_U(b0[3], 0xd6000000u);
    CHECK_EQ_U(b1[0], 0xc0000000u);
    for (c = 1; c < 4; c++) {
        /* D:287-288 "All slaves share the same memBase1 address space",
         * master memBase1 + its (narrowed) 64 MB */
        CHECK_EQ_U(b1[c], 0xc0000000u + (64u << 20));
        CHECK_EQ_U(CFG(m, c, 0x10), b0[c]);
        CHECK_EQ_U(CFG(m, c, 0x14) & ~0xfu, 0xc4000000u);
        /* D:289-290 the slaves share the master's I/O BAR ... */
        CHECK_EQ_U(CFG(m, c, 0x18) & ~0x3u, 0xc000u);
        /* ... with I/O off and memory on (D:315-319) */
        CHECK_EQ_U(CFG(m, c, 0x04) & 3, VCR_SLI_CMD_MEM);
        /* 32 MB memBase0, 64 MB memBase1, snoop decodes 32 MB / 64 MB: the
         * exact value the vendor driver leaves (measured 0x00011445) */
        CHECK_EQ_U(CFG(m, c, 0x48), 0x00011445u);
        /* init and PCI FIFO writes on, BAR writes on only while the BARs
         * were placed (the vendor's state, measured 0x301); fab ID kept */
        CHECK_EQ_U(CFG(m, c, 0x40), 0x00000301u);
    }
    /* already narrowed by the miniport: the master's decode is not rewritten */
    CHECK_EQ_U(count_w(m, 'c', 0, 0x48), 0);
    CHECK_EQ_U(CFG(m, 0, 0x48), 0x45u);
    CHECK_EQ_U(CFG(m, 0, 0x04) & 3, 3);     /* master keeps I/O + memory */
    no_bus_faults(m);
}

TEST(map_narrows_a_master_still_in_its_power_up_decode) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_u32 b0[4], b1[4];
    seed(m, 4);
    m->cfg[0][CFGI(0x48)] = 0x10;           /* BIOS: 128 MB / 256 MB (measured) */
    io = mkio(m);
    CHECK_EQ_I(vcr_sli_map_slaves(&io, 4, b0, b1), VCR_SLI_OK);
    CHECK_EQ_U(count_w(m, 'c', 0, 0x48), 1);
    CHECK_EQ_U(CFG(m, 0, 0x48), 0x45u);     /* 32 MB / 64 MB / 256 B */
    CHECK_EQ_U(CFG(m, 3, 0x48), 0x00011445u);
    CHECK_EQ_U(b1[2], 0xc4000000u);         /* +64 MB, not +256 MB */
    no_bus_faults(m);
}

TEST(map_refuses_a_missing_slave_and_writes_nothing) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_u32 b0[4], b1[4];
    seed(m, 4);
    m->present[3] = 0;
    io = mkio(m);
    CHECK_EQ_I(vcr_sli_map_slaves(&io, 4, b0, b1), VCR_SLI_ENODEV);
    CHECK_EQ_U(m->nw, 0);
    CHECK(has_step(m, VCR_SLI_S_REFUSED), "refusal not logged");
    CHECK_EQ_I(vcr_sli_map_slaves(&io, 3, b0, b1), VCR_SLI_EINVAL);
    CHECK_EQ_U(m->nw, 0);
    /* a 2-chip board maps just the one slave */
    CHECK_EQ_I(vcr_sli_map_slaves(&io, 2, b0, b1), VCR_SLI_OK);
    CHECK_EQ_U(b0[1], 0xd2000000u);
    CHECK_EQ_U(count_w(m, 'c', 2, 0x10), 0);
}

/* ---- initSlave ------------------------------------------------------------------- */

TEST(init_copies_the_master_and_hands_the_io_bar_back_every_time) {
    mock *m = &M;
    vcr_sli_io io;
    const vcr_u32 rst = 0xf3;       /* grx, FIFO, video, 2D, memory + VGA timing */
    unsigned i, k, ncmd = 0;
    vcr_u32 cmds[64][3];
    int c;
    mapped(m, &io, 4);
    m->nw = 0;
    CHECK_EQ_I(vcr_sli_init_slaves(&io, 4), VCR_SLI_OK);
    for (c = 1; c < 4; c++) {
        int saw_on = 0, saw_off_after = 0;
        CHECK_EQ_U(IOR(m, c, VCR_R_PLLCTRL1), IOR(m, 0, VCR_R_PLLCTRL1));
        CHECK_EQ_U(IOR(m, c, VCR_R_DRAMINIT0), IOR(m, 0, VCR_R_DRAMINIT0));
        CHECK_EQ_U(IOR(m, c, VCR_R_DRAMINIT1), IOR(m, 0, VCR_R_DRAMINIT1));
        CHECK_EQ_U(IOR(m, c, VCR_R_PCIINIT0), M_PCIINIT0);
        CHECK_EQ_U(IOR(m, c, VCR_R_TMUGBEINIT), M_TMUGBEINIT);
        CHECK_EQ_U(IOR(m, c, VCR_R_DRAMDATA), 0x37);
        CHECK_EQ_U(IOR(m, c, VCR_R_DRAMCOMMAND), 0x10d);
        /* D:346-349 bit 30 off on master AND slave; the reset pulse returns
         * miscInit0 to that value */
        CHECK_EQ_U(IOR(m, c, VCR_R_MISCINIT0), M_MISCINIT0 & ~(1u << 30));
        /* h3InitVga(legacy decode off): extensions, 3C3 wake-up, decode off */
        CHECK_EQ_U(IOR(m, c, VCR_R_VGAINIT0), 0x340);
        CHECK_EQ_U(IOR(m, c, VCR_R_VGAINIT1), 0);
        CHECK_EQ_U(IOR(m, c, VCR_R_MISCINIT1), M_MISCINIT1 | 1);
        CHECK_EQ_U(m->wake[c], 1);          /* reached the SLAVE, not the master */
        /* the reset really pulsed: all six bits set, then all clear */
        for (i = 0; i < m->nw; i++) {
            if (m->w[i].kind != 'i' || m->w[i].chip != (vcr_u32)c ||
                m->w[i].off != VCR_R_MISCINIT0)
                continue;
            if ((m->w[i].val & rst) == rst)
                saw_on = 1;
            else if (saw_on && !(m->w[i].val & rst))
                saw_off_after = 1;
        }
        CHECK(saw_on && saw_off_after, "no reset pulse on the slave");
        CHECK_EQ_U(CFG(m, c, 0x04) & 1, 0);  /* slave I/O decode off again */
    }
    CHECK_EQ_U(IOR(m, 0, VCR_R_MISCINIT0), M_MISCINIT0 & ~(1u << 30));
    CHECK_EQ_U(m->wake[0], 0);
    CHECK_EQ_U(CFG(m, 0, 0x04) & 3, 3);      /* the master has its I/O BAR back */

    /* the toggling, exactly as D:359-381: per slave, master off -> slave on
     * -> (slave VGA) -> slave off -> master on */
    for (i = 0; i < m->nw && ncmd < 64; i++)
        if (m->w[i].kind == 'c' && m->w[i].off == 0x04) {
            cmds[ncmd][0] = m->w[i].chip;
            cmds[ncmd][1] = m->w[i].val & 1;
            cmds[ncmd][2] = i;
            ncmd++;
        }
    CHECK_EQ_U(ncmd, 12);
    for (k = 0; k + 3 < ncmd && k < 12; k += 4) {
        vcr_u32 s = 1 + k / 4;
        CHECK(cmds[k][0] == 0 && cmds[k][1] == 0, "master I/O not turned off first");
        CHECK(cmds[k + 1][0] == s && cmds[k + 1][1] == 1, "slave I/O not turned on");
        CHECK(cmds[k + 2][0] == s && cmds[k + 2][1] == 0, "slave I/O not turned off");
        CHECK(cmds[k + 3][0] == 0 && cmds[k + 3][1] == 1, "master I/O not restored");
        /* the slave's 0x3c3 write sits inside its window */
        for (i = 0; i < m->nw; i++)
            if (m->w[i].kind == 'v' && m->w[i].chip == s && m->w[i].off == 0x3c3)
                CHECK(i > cmds[k + 1][2] && i < cmds[k + 2][2], "VGA access outside window");
    }
    no_bus_faults(m);
}

/* ---- hwcSetSLIAAMode: 4-chip analog SLI, the V5 6000's own mode ---------------- */

TEST(four_chip_analog_sli_programs_every_chip) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_sli_aa_req r = req(4, 1, 0, 0, 1, 32, 16);
    static const vcr_u8 idx[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                    0x09, 0x10, 0x11, 0x12, 0x15, 0x16, 0x1a, 0x1b };
    vcr_u32 c;
    int i;
    mapped(m, &io, 4);
    /* no 6000 clock yet: done, and it says so */
    CHECK_EQ_I(vcr_sli_set(&io, &r), VCR_SLI_W_NOCLOCK);
    CHECK(has_step(m, VCR_SLI_S_CLOCK_6K), "6000 clock hook not reported");

    for (c = 0; c < 4; c++) {
        /* 3D sliCtrl (gsst.c): render 3<<5, compare chip<<5, scan 31, 4 chips */
        CHECK_EQ_U(m->slictrl[c], 0x061f0060u | (c << 13));
        CHECK_EQ_U(m->slictrl_direct[c], 1);
        /* cfgSliLfbCtrl: same masks + CPU write, dispatch write, read */
        CHECK_EQ_U(CFG(m, c, VCR_CFG_SLILFBCTRL), 0x1e1f0060u | (c << 13));
        /* D:1019-1044: 4-way analog SLI video mux, PLL from sync_clk (D:1453/1465) */
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL0), c ? 0x803u : 0x801u);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL1),
                   c ? (0x00600060u | (c << 13) | (c << 29)) : 0x00600060u);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL2), 0xff00u);
        /* no AA: the AA apertures are not touched */
        CHECK_EQ_U(CFG(m, c, VCR_CFG_AALFBCTRL), 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_AADEPTHBUFAPERTURE), 0);
        /* D:730-768 */
        CHECK_EQ_U(IOR(m, c, VCR_R_PCIINIT0), 0x00000303u);
        CHECK_EQ_U(IOR(m, c, VCR_R_TMUGBEINIT), 0x00500f00u);
        /* slaves: RAMDAC off (D:1460-1463); the master's stays on */
        CHECK_EQ_U(IOR(m, c, VCR_R_MISCINIT1) & VCR_MI1_POWERDOWN_DAC, c ? VCR_MI1_POWERDOWN_DAC : 0);
    }
    /* swap control and snooping (D:770-804) */
    CHECK_EQ_U(CFG(m, 0, 0x40), 0x06000b01u);   /* swap master + algorithm, address snoop */
    for (c = 1; c < 4; c++) {
        /* snoop the master's memBase0 (0xd0000000 >> 22 = 0x340), memBase1,
         * init registers; quick sampling (> 2 chips); swap algorithm */
        CHECK_EQ_U(CFG(m, c, 0x40), 0x4ba07b01u);
        CHECK_EQ_U(CFG(m, c, 0x48), 0x0c011445u);   /* + memBase1 snoop 0xc0000000 */
        /* D:887-900 analog: slave vsync 7 pixels / 4 chars early; the
         * read-modify-write keeps the power-up bit 11 */
        CHECK_EQ_U(CFG(m, c, VCR_CFG_SLIAAMISC), 0x827u);
    }
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_SLIAAMISC), 0x800u);     /* untouched power-up value */

    /* the master's video mode, copied to each slave (D:432-682) */
    for (c = 1; c < 4; c++) {
        for (i = 0; i < 16; i++)
            CHECK_EQ_U(m->crtc[c][idx[i]], m->crtc[0][idx[i]]);
        CHECK_EQ_U(m->crtc[c][0x17], 0x80);     /* sync outputs on */
        CHECK_EQ_U(m->misc[c], 0x2f | 1);
        CHECK_EQ_U(m->seq[c][1], 0x21 & ~0x20); /* screen on */
        CHECK_EQ_U(m->seq[c][0], 0x03);
        CHECK_EQ_U(m->attr[c][0x10], 0x01);
        CHECK_EQ_U(m->attr[c][0x12], 0x0f);
        CHECK_EQ_U(IOR(m, c, VCR_R_PLLCTRL0), M_PLLCTRL0);
        CHECK_EQ_U(IOR(m, c, VCR_R_DACMODE), 0);
        CHECK_EQ_U(IOR(m, c, VCR_R_VIDSCREENSIZE), M_SCREENSIZE);
        CHECK_EQ_U(IOR(m, c, VCR_R_VIDPROCCFG), M_VIDPROCCFG | 1);
        CHECK_EQ_U(IOR(m, c, VCR_R_VIDDESKTOPOVERLAYSTRIDE), 0x500);
        CHECK_EQ_U(IOR(m, c, VCR_R_VIDMAXRGBDELTA), 0x00100810u);
        CHECK(IOR(m, c, VCR_R_VGAINIT0) & (1u << 12), "slave VGA refresh left on");
        CHECK_EQ_U(CFG(m, c, 0x04) & 1, 0);
    }
    /* the master's own mode was only read */
    CHECK_EQ_U(m->crtc[0][0x17], 0x40 + 0x17);
    CHECK_EQ_U(CFG(m, 0, 0x04) & 3, 3);
    no_bus_faults(m);
}

TEST(disable_zeroes_sli_aa_config_and_tristates_the_slaves) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_sli_aa_req on = req(4, 1, 0, 0, 1, 32, 16), off;
    vcr_u32 c;
    mapped(m, &io, 4);
    CHECK(vcr_sli_set(&io, &on) >= 0, "enable failed");
    /* Glide's disable request: dwChips, sliEn = aaEn = 0, the rest garbage */
    memset(&off, 0xa5, sizeof off);
    off.ChipInfo.dwChips = 4;
    off.ChipInfo.dwsliEn = 0;
    off.ChipInfo.dwaaEn = 0;
    m->unlogged = 0;
    CHECK_EQ_I(vcr_sli_set(&io, &off), VCR_SLI_OK);
    for (c = 0; c < 4; c++) {
        CHECK_EQ_U(m->slictrl[c], 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_SLILFBCTRL), 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_AALFBCTRL), 0);
        /* the SLI/AA fields cleared, the power-up bit 11 (0x800) kept */
        CHECK_EQ_U(CFG(m, c, VCR_CFG_SLIAAMISC), 0x800u);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL1), 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL2), 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_AADEPTHBUFAPERTURE), 0);
        /* D:1494-1499 slaves must not drive HSYNC/VSYNC */
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL0), c ? 0x03000000u : 0);
        /* snooping and swap control gone; the rest of cfgInitEnable kept */
        CHECK_EQ_U(CFG(m, c, 0x40), 0x00000301u);
        if (c) {
            CHECK_EQ_U(IOR(m, c, VCR_R_DACMODE), 0xa);      /* DPMS both syncs */
            CHECK_EQ_U(IOR(m, c, VCR_R_VIDPROCCFG) & 1, 0);
        }
    }
    no_bus_faults(m);
}

/* ---- the vendor driver, measured ---------------------------------------------------
 * AmigaMerlin 3.1-R11 on .124 with our Glide running Quake II in 4-chip SLI
 * at 640x480 (golden/sli_amigamerlin-3.1-r11_cfg5_192.168.1.124.json, captured
 * 2026-09-26 by tools/sli_golden.py: PCI config through raw 0xCF8 cycles, IO
 * registers through each chip's BAR0). The same request through our port
 * must leave every SLI register the vendor's kernel driver left. Band height
 * 8 lines (cfgVideoCtrl1 render mask 0x18 = 3 << 3). */
TEST(four_chip_sli_config_space_equals_the_vendor_driver_on_124) {
    static const vcr_u32 ie40[4] = { 0x06000b01u, 0x4ba07b01u, 0x4ba07b01u, 0x4ba07b01u };
    static const vcr_u32 dec48[4] = { 0x00000045u, 0x0c011445u, 0x0c011445u, 0x0c011445u };
    static const vcr_u32 vc0[4] = { 0x00000801u, 0x00000803u, 0x00000803u, 0x00000803u };
    static const vcr_u32 vc1[4] = { 0x00180018u, 0x08180818u, 0x10181018u, 0x18181818u };
    static const vcr_u32 slilfb[4] = { 0x1e070018u, 0x1e070818u, 0x1e071018u, 0x1e071818u };
    static const vcr_u32 sliaa[4] = { 0x00000800u, 0x00000827u, 0x00000827u, 0x00000827u };
    static const vcr_u32 bar0[4] = { 0xd0000000u, 0xd2000000u, 0xd4000000u, 0xd6000000u };
    mock *m = &M;
    vcr_sli_io io;
    vcr_sli_aa_req r = req(4, 1, 0, 0, 1, 8, 16);
    vcr_u32 c;
    mapped(m, &io, 4);
    m->io[0][VCR_R_TMUGBEINIT >> 2] = 0x00000ff0u;      /* the master before SLI (vcrkmd golden) */
    m->io[0][VCR_R_VGAINIT0 >> 2] = 0x00001140u;
    /* the chips' own strap bits as .124 reads them before SLI: chip 1 0x26..,
     * chips 2 and 3 0x36.. (the master 0x26..) */
    m->io[0][VCR_R_MISCINIT1 >> 2] = 0x26000000u | (m->io[0][VCR_R_MISCINIT1 >> 2] & 0xffffffu);
    m->io[1][VCR_R_MISCINIT1 >> 2] = 0x26000000u;
    m->io[2][VCR_R_MISCINIT1 >> 2] = 0x36000000u;
    m->io[3][VCR_R_MISCINIT1 >> 2] = 0x36000000u;
    m->io[0][VCR_R_VIDPIXELBUFTHOLD >> 2] = 0x00020820u;  /* Glide's own, as at 1600x1200 */
    m->io[0][VCR_R_VIDDESKTOPSTARTADDR >> 2] = 0x01ec0000u;
    CHECK(vcr_sli_set(&io, &r) >= 0, "enable failed");
    for (c = 0; c < 4; c++) {
        CHECK_EQ_U(CFG(m, c, 0x04) & 0xffff, c ? 0x0002u : 0x0003u);
        CHECK_EQ_U(CFG(m, c, 0x10), bar0[c]);
        CHECK_EQ_U(CFG(m, c, 0x14), c ? 0xc4000008u : 0xc0000008u);
        CHECK_EQ_U(CFG(m, c, 0x18), 0x0000c001u);
        CHECK_EQ_U(CFG(m, c, 0x40), ie40[c]);
        CHECK_EQ_U(CFG(m, c, 0x48), dec48[c]);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL0), vc0[c]);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL1), vc1[c]);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL2), 0x0000ff00u);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_SLILFBCTRL), slilfb[c]);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_AADEPTHBUFAPERTURE), 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_AALFBCTRL), 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_SLIAAMISC), sliaa[c]);
        CHECK_EQ_U(IOR(m, c, VCR_R_TMUGBEINIT), 0x00500ff0u);
        CHECK_EQ_U(IOR(m, c, VCR_R_VGAINIT0), c ? 0x00001340u : 0x00001140u);
        CHECK_EQ_U(IOR(m, c, VCR_R_MISCINIT1) & VCR_MI1_POWERDOWN_DAC, c ? VCR_MI1_POWERDOWN_DAC : 0);
        /* vendor: slaves keep their straps (dos_mode.c would copy the master's
         * 0x26 onto chips 2 and 3), and carry the master's pixel-buffer
         * threshold and desktop start - 2026-09-26 live diff on .124 */
        CHECK_EQ_U(IOR(m, c, VCR_R_MISCINIT1) >> 24, c >= 2 ? 0x36u : 0x26u);
        CHECK_EQ_U(IOR(m, c, VCR_R_VIDPIXELBUFTHOLD), c ? 0x00010410u : 0x00020820u);
        CHECK_EQ_U(IOR(m, c, VCR_R_VIDDESKTOPSTARTADDR), 0x01ec0000u);
    }
    no_bus_faults(m);
}

/* ---- AA: the branch conditions nobody can eyeball ------------------------------- */

TEST(four_chip_4_sample_analog_aa_follows_the_vsync_and_mux_branches) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_sli_aa_req r = req(4, 0, 1, 1, 1, 32, 32);
    vcr_u32 c;
    r.MemInfo.dwaaSecondaryColorBufBegin = 0x00100000u;
    r.MemInfo.dwaaSecondaryDepthBufBegin = 0x01000000u;
    r.MemInfo.dwaaSecondaryDepthBufEnd = 0x01400000u;
    mapped(m, &io, 4);
    CHECK_EQ_I(vcr_sli_set(&io, &r), VCR_SLI_W_NOCLOCK);
    for (c = 0; c < 4; c++) {
        CHECK_EQ_U(CFG(m, c, VCR_CFG_SLILFBCTRL), 0);       /* SLI off, AA on */
        /* D:871 as ported (byte address << 4 - UNVERIFIED, see vcrmp_sli.c),
         * CPU + dispatch write, 32 bpp, divide by 4 */
        CHECK_EQ_U(CFG(m, c, VCR_CFG_AALFBCTRL), 0x01000000u | 0x0c000000u | 0x40000000u | 0x80000000u);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_AADEPTHBUFAPERTURE), 0x1000u | (0x1400u << 16));
        CHECK_EQ_U(m->slictrl_direct[c], 0);                /* no SLI: no sliCtrl */
    }
    /* D:1140-1187 */
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_VIDEOCTRL0), 0x2811u);         /* EN, /4, pipe+AA fifo, PLL sel */
    CHECK_EQ_U(CFG(m, 1, VCR_CFG_VIDEOCTRL0), 0x02000803u);     /* slave, hsync tristate */
    CHECK_EQ_U(CFG(m, 2, VCR_CFG_VIDEOCTRL0), 0x02002843u);
    CHECK_EQ_U(CFG(m, 3, VCR_CFG_VIDEOCTRL0), 0x02000803u);
    CHECK_EQ_U(CFG(m, 1, VCR_CFG_VIDEOCTRL1), 0xff000000u);
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_VIDEOCTRL2), 0);               /* 1x video */
    CHECK_EQ_U(CFG(m, 2, VCR_CFG_VIDEOCTRL2), 0xff00u);
    /* D:887-901: chips 1 and 3 run 8 clocks ahead, chip 2 takes the analog
     * offset - the one condition in the file that reads vid2xMode */
    CHECK_EQ_U(CFG(m, 1, VCR_CFG_SLIAAMISC), 0x82fu);
    CHECK_EQ_U(CFG(m, 2, VCR_CFG_SLIAAMISC), 0x827u);
    CHECK_EQ_U(CFG(m, 3, VCR_CFG_SLIAAMISC), 0x82fu);
    no_bus_faults(m);
}

TEST(two_chip_digital_sli_and_no_clock_hook_on_a_two_chip_board) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_sli_aa_req r = req(2, 1, 0, 0, 0, 16, 16);
    mapped(m, &io, 2);                      /* a V5 5500: chips 2 and 3 absent */
    CHECK_EQ_I(vcr_sli_set(&io, &r), VCR_SLI_OK);
    CHECK(!has_step(m, VCR_SLI_S_CLOCK_6K), "6000 clock hook called on a 2-chip board");
    CHECK_EQ_U(m->slictrl[0], 0x050f0010u);
    CHECK_EQ_U(m->slictrl[1], 0x050f1010u);
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_SLILFBCTRL), 0x1d0f0010u);
    CHECK_EQ_U(CFG(m, 1, VCR_CFG_SLILFBCTRL), 0x1d0f1010u);
    /* D:992-1018 */
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_VIDEOCTRL0), 0x21u);           /* no PLL sel on a 2-chip master */
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_VIDEOCTRL1), 0x10u);
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_VIDEOCTRL2), 0x1010u);
    CHECK_EQ_U(CFG(m, 1, VCR_CFG_VIDEOCTRL0), 0x803u);
    CHECK_EQ_U(CFG(m, 1, VCR_CFG_VIDEOCTRL1), 0xff001010u);
    CHECK_EQ_U(CFG(m, 1, VCR_CFG_VIDEOCTRL2), 0x1010u);
    CHECK_EQ_U(CFG(m, 1, VCR_CFG_SLIAAMISC), 0x82fu);           /* digital: 8 clocks ahead */
    CHECK_EQ_U(CFG(m, 1, 0x40) & (VCR_IE_QUICK_SAMPLING << 8), 0);  /* only > 2 chips */
    no_bus_faults(m);
}

/* ---- bounded: a register that never changes cannot hang the kernel -------------- */

TEST(a_status_that_never_frees_cannot_hang_init_or_the_sli_enable) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_sli_aa_req r = req(4, 1, 0, 0, 1, 32, 16);
    mapped(m, &io, 4);
    m->status_stuck[2] = 1;
    CHECK_EQ_I(vcr_sli_init_slaves(&io, 4), VCR_SLI_W_TIMEOUT);
    /* four bounded CHECKFORROOMs + one read-back, and then it gave up */
    CHECK(m->status_reads[2] >= VCR_SLI_ROOM_POLLS, "did not wait at all");
    CHECK(m->status_reads[2] <= 4ul * VCR_SLI_ROOM_POLLS + 1, "wait not bounded");
    CHECK(m->stall_calls <= 4ul * VCR_SLI_ROOM_POLLS + 16, "stalls not bounded");
    CHECK(has_step(m, VCR_SLI_S_TIMEOUT), "timeout not logged");
    CHECK_EQ_U(IOR(m, 2, VCR_R_VGAINIT0), 0);       /* the guarded write was skipped */
    CHECK_EQ_U(IOR(m, 1, VCR_R_VGAINIT0), 0x340);   /* the others were not */
    /* and the shared I/O BAR went back to the master regardless */
    CHECK_EQ_U(CFG(m, 2, 0x04) & 1, 0);
    CHECK_EQ_U(CFG(m, 0, 0x04) & 1, 1);

    /* the same stuck chip during an enable: its sliCtrl is skipped, not waited
     * on for ever, and the result says so */
    CHECK(vcr_sli_set(&io, &r) & VCR_SLI_W_TIMEOUT, "enable hid the timeout");
    CHECK_EQ_U(m->slictrl_direct[2], 0);
    CHECK_EQ_U(m->slictrl_direct[1], 1);
    CHECK_EQ_U(m->slictrl_direct[3], 1);
    no_bus_faults(m);
}

/* ---- refusals write nothing ------------------------------------------------------ */

TEST(bad_requests_are_refused_before_the_first_write) {
    mock *m = &M;
    vcr_sli_io io, broken;
    vcr_sli_aa_req r;
    mapped(m, &io, 4);
    m->nw = 0;
    r = req(4, 1, 0, 0, 1, 24, 16);                 /* band height not a power of 2 */
    CHECK_EQ_I(vcr_sli_set(&io, &r), VCR_SLI_EINVAL);
    r = req(3, 1, 0, 0, 1, 32, 16);                 /* three chips */
    CHECK_EQ_I(vcr_sli_set(&io, &r), VCR_SLI_EINVAL);
    r = req(4, 0, 1, 0, 1, 32, 8);                  /* AA at 8 bpp */
    CHECK_EQ_I(vcr_sli_set(&io, &r), VCR_SLI_EINVAL);
    r = req(4, 0, 1, 3, 1, 32, 16);                 /* 16-sample AA */
    CHECK_EQ_I(vcr_sli_set(&io, &r), VCR_SLI_EINVAL);
    r = req(1, 1, 0, 0, 0, 32, 16);                 /* SLI on one chip */
    CHECK_EQ_I(vcr_sli_set(&io, &r), VCR_SLI_EINVAL);
    CHECK_EQ_U(m->nw, 0);
    CHECK(has_step(m, VCR_SLI_S_REFUSED), "refusal not logged");
    /* a missing slave */
    m->present[3] = 0;
    r = req(4, 1, 0, 0, 1, 32, 16);
    CHECK_EQ_I(vcr_sli_set(&io, &r), VCR_SLI_ENODEV);
    CHECK_EQ_U(m->nw, 0);
    /* a broken accessor table, or none */
    broken = io;
    broken.vga_wr = NULL;
    CHECK_EQ_I(vcr_sli_set(&broken, &r), VCR_SLI_EINVAL);
    CHECK_EQ_I(vcr_sli_set(NULL, &r), VCR_SLI_EINVAL);
    CHECK_EQ_I(vcr_sli_set(&io, NULL), VCR_SLI_EINVAL);
    CHECK_EQ_I(vcr_sli_init_slaves(NULL, 4), VCR_SLI_EINVAL);
    CHECK_EQ_U(m->nw, 0);
}

/* ---- the placeholders and pure values -------------------------------------------- */

TEST(the_6000_clock_is_a_visible_placeholder) {
    mock *m = &M;
    vcr_sli_io io;
    unsigned i;
    int found = 0;
    vcr_sli_aa_req r = req(4, 1, 0, 0, 1, 32, 16);
    CHECK_EQ_I(vcr_sli_6k_clock(NULL), VCR_SLI_ENOTIMPL);
    mapped(m, &io, 4);
    CHECK(vcr_sli_set(&io, &r) & VCR_SLI_W_NOCLOCK, "missing clock not reported");
    for (i = 0; i < m->nl; i++)
        if (m->l[i].step == VCR_SLI_S_CLOCK_6K) {
            found = 1;
            CHECK_EQ_I((int)m->l[i].val, VCR_SLI_ENOTIMPL);
        }
    CHECK(found, "no CLOCK_6K step");
    /* before any slave was touched (D:617-626) */
    for (i = 0; i < m->nl && m->l[i].step != VCR_SLI_S_CLOCK_6K; i++)
        CHECK(m->l[i].step != VCR_SLI_S_INIT_BEGIN, "clock hook after initSlave");
}

TEST(slictrl_values_follow_gsst) {
    /* 4 chips, no AA, 32 lines */
    CHECK_EQ_U(vcr_sli_slictrl(4, 32, 0, 0, 0), 0x061f0060u);
    CHECK_EQ_U(vcr_sli_slictrl(4, 32, 0, 0, 3), 0x061f6060u);
    /* 4 chips, 2-sample AA: two 2-way units, chips 2/3 own the odd bands */
    CHECK_EQ_U(vcr_sli_slictrl(4, 32, 1, 0, 1), 0x051f0020u);
    CHECK_EQ_U(vcr_sli_slictrl(4, 32, 1, 0, 2), 0x051f2020u);
    /* nothing to program */
    CHECK_EQ_U(vcr_sli_slictrl(1, 32, 0, 0, 0), 0);
    CHECK_EQ_U(vcr_sli_slictrl(2, 32, 1, 1, 0), 0);     /* 2 chips 4-sample: 1 unit */
    CHECK_EQ_U(vcr_sli_slictrl(4, 24, 0, 0, 0), 0);     /* bad band height */
    CHECK_EQ_U(vcr_sli_slictrl(4, 32, 0, 0, 4), 0);     /* no such chip */
}

TEST(step_codes_are_unique) {
#define VCR_SLI_STEP(name, code, desc) code,
    static const int codes[] = { VCR_SLI_STEP_TABLE };
#undef VCR_SLI_STEP
    unsigned i, j, n = sizeof codes / sizeof codes[0];
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            CHECK(codes[i] != codes[j], "duplicate SLI step code");
}

MUNIT_MAIN("vcr-kmd SLI/AA bring-up (vcrmp_sli.c)",
    RUN(map_puts_each_slave_32mb_above_the_last_and_all_bar1s_above_the_master);
    RUN(map_narrows_a_master_still_in_its_power_up_decode);
    RUN(map_refuses_a_missing_slave_and_writes_nothing);
    RUN(init_copies_the_master_and_hands_the_io_bar_back_every_time);
    RUN(four_chip_analog_sli_programs_every_chip);
    RUN(disable_zeroes_sli_aa_config_and_tristates_the_slaves);
    RUN(four_chip_sli_config_space_equals_the_vendor_driver_on_124);
    RUN(four_chip_4_sample_analog_aa_follows_the_vsync_and_mux_branches);
    RUN(two_chip_digital_sli_and_no_clock_hook_on_a_two_chip_board);
    RUN(a_status_that_never_frees_cannot_hang_init_or_the_sli_enable);
    RUN(bad_requests_are_refused_before_the_first_write);
    RUN(the_6000_clock_is_a_visible_placeholder);
    RUN(slictrl_values_follow_gsst);
    RUN(step_codes_are_unique);
)
