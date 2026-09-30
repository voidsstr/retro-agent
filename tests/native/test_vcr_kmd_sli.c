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
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "munit.h"
#include "../../voodoo-cleanroom/vcr-kmd/miniport/vcrmp_sli.c"
#include "../../voodoo-cleanroom/vcr-kmd/tools/vcr_sliaa.h"     /* `vcrctl sliaa` */
/* VCR_PCI_CF8: what a raw slave config cycle keeps of an offset (the rest of
 * the header is 32-bit kernel asm, never emitted here) */
#include "../../voodoo-cleanroom/vcr-kmd/include/vcr_pciraw.h"

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
    /* every access, by kind: the AA read-back must be config READS only */
    unsigned long cfg_reads, bar_ops;
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
    m->cfg_reads++;
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
    m->bar_ops++;
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
    m->bar_ops++;
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
    int d;
    m->bar_ops++;
    d = vga_target(m, chip);
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

    m->bar_ops++;
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

/* ---- the 256 MB VBIOS mode (64 MB a chip), 2026-09-27 ------------------------------
 * memBase1 decodes TWICE a chip's memory. dos_mode.c (and vcr-kmd until
 * 2026-09-27) hard-coded 64 MB - the 32 MB/chip board's value - so on the
 * 6000's 256 MB mode the tiled half was cut off and every slave's shared
 * memBase1 (master + mb1) landed on the master's upper 64 MB. */
TEST(the_decode_index_is_twice_the_memory_as_a_cfgPciDecode_index) {
    CHECK_EQ_I(vcr_pcidec_index(2ul * (32u << 20)), 4);     /* 128 MB board: 64 MB */
    CHECK_EQ_I(vcr_pcidec_index(2ul * (64u << 20)), 0);     /* 256 MB mode: 128 MB */
    CHECK_EQ_I(vcr_pcidec_index(2ul * (16u << 20)), 5);
    CHECK_EQ_I(vcr_pcidec_index(256ul << 20), 1);
    CHECK_EQ_I(vcr_pcidec_index(4ul << 20), 8);
    CHECK_EQ_I(vcr_pcidec_index(48ul << 20), -1);           /* not a power of two */
    CHECK_EQ_I(vcr_pcidec_index((64ul << 20) + 4096), -1);
    CHECK_EQ_I(vcr_pcidec_index(2ul << 20), -1);            /* below the field */
    CHECK_EQ_I(vcr_pcidec_index(0), -1);
}

TEST(an_explicit_32mb_chip_maps_exactly_as_the_legacy_default) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_u32 b0[4], b1[4];
    seed(m, 4);
    m->cfg[0][CFGI(0x48)] = 0x10;
    io = mkio(m);
    io.fb_bytes = 32u << 20;
    CHECK_EQ_I(vcr_sli_map_slaves(&io, 4, b0, b1), VCR_SLI_OK);
    CHECK_EQ_U(CFG(m, 0, 0x48), 0x45u);
    CHECK_EQ_U(CFG(m, 3, 0x48), 0x00011445u);
    CHECK_EQ_U(b1[3], 0xc0000000u + (64u << 20));
    no_bus_faults(m);
}

TEST(a_64mb_chip_decodes_128mb_and_the_slaves_sit_128mb_above_the_master) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_u32 b0[4], b1[4];
    int c;
    seed(m, 4);
    m->cfg[0][CFGI(0x48)] = 0x10;           /* power-up decode */
    io = mkio(m);
    io.fb_bytes = 64u << 20;
    CHECK_EQ_I(vcr_sli_map_slaves(&io, 4, b0, b1), VCR_SLI_OK);
    /* master: 32 MB memBase0, 128 MB (index 0) memBase1, 256 B I/O */
    CHECK_EQ_U(CFG(m, 0, 0x48), 0x05u);
    for (c = 1; c < 4; c++) {
        CHECK_EQ_U(b0[c], 0xd0000000u + (32u << 20) * (vcr_u32)c);   /* registers unchanged */
        CHECK_EQ_U(b1[c], 0xc0000000u + (128u << 20));                /* was + 64 MB */
        CHECK_EQ_U(CFG(m, c, 0x14) & ~0xfu, 0xc8000000u);
        /* snoop memBase0 32 MB (5 << 10), snoop memBase1 128 MB (0 << 14) */
        CHECK_EQ_U(CFG(m, c, 0x48), 0x00001405u);
    }
    no_bus_faults(m);
}

TEST(a_decode_narrowed_for_32mb_is_widened_for_a_64mb_chip) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_u32 b0[4], b1[4];
    seed(m, 4);
    /* what vcr-kmd before 2026-09-27 left at boot on ANY board: 32 / 64 MB */
    m->cfg[0][CFGI(0x48)] = 0x45;
    io = mkio(m);
    io.fb_bytes = 64u << 20;
    CHECK_EQ_I(vcr_sli_map_slaves(&io, 4, b0, b1), VCR_SLI_OK);
    CHECK_EQ_U(count_w(m, 'c', 0, 0x48), 1);
    CHECK_EQ_U(CFG(m, 0, 0x48), 0x05u);
    CHECK_EQ_U(b1[1], 0xc8000000u);
    /* the legacy default (fb_bytes 0) leaves the same 0x45 alone - the old
     * value, kept exactly for a 32 MB board */
    seed(m, 4);
    m->cfg[0][CFGI(0x48)] = 0x45;
    io = mkio(m);
    CHECK_EQ_I(vcr_sli_map_slaves(&io, 4, b0, b1), VCR_SLI_OK);
    CHECK_EQ_U(count_w(m, 'c', 0, 0x48), 0);
    CHECK_EQ_U(b1[1], 0xc4000000u);
    no_bus_faults(m);
}

TEST(a_memory_size_the_decode_cannot_express_is_refused_with_nothing_written) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_u32 b0[4], b1[4], c;
    seed(m, 4);
    m->cfg[0][CFGI(0x48)] = 0x10;
    io = mkio(m);
    io.fb_bytes = 48u << 20;
    CHECK(vcr_sli_map_slaves(&io, 4, b0, b1) < 0, "48 MB a chip accepted");
    for (c = 0; c < 4; c++)
        CHECK_EQ_U(count_w(m, 'c', c, 0x48), 0);
    CHECK_EQ_U(CFG(m, 0, 0x48), 0x10u);
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
        /* the DEFAULT recipe is D:871 as 097b1f7 ported it - the byte address
         * << 4, CPU + dispatch write, 32 bpp, divide by 4 - the control arm of
         * an AA A/B. (cebdf4f made the byte-address base unconditional:
         * 0x4c100000 here; it is the vendor recipe's since, header difference
         * 13 - the_aa_base_is_a_byte_address_in_the_vendor_recipe_only) */
        CHECK_EQ_U(CFG(m, c, VCR_CFG_AALFBCTRL), 0x01000000u | 0x0c000000u | 0x40000000u | 0x80000000u);
        CHECK(CFG(m, c, VCR_CFG_AALFBCTRL) != (0x00100000u | 0x0c000000u | 0x40000000u | 0x80000000u),
              "the default recipe writes the vendor recipe's base");
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

/* ---- the proven sequences, pinned byte for byte ------------------------------------
 * Every bus write (kind, chip, offset, value) of a request, in order, folded
 * into FNV-1a, plus the write count and the result. Recorded from vcrmp_sli.c
 * as it stood at 097b1f7 - the code that ran cfg 0/2/5 on .124 (4-chip SLI
 * parity with AmigaMerlin, 0 bad band lines) - BEFORE the AA safety net
 * (2026-09-27). The safety net may only ADD refusals; for every request it
 * accepts, the bus must see exactly what it saw before - no exception: the AA
 * base fix is the vendor recipe's (header difference 13). The vendor AA
 * recipe (vcr_sli_set_ex flag) is OFF here: these are the defaults. */
static vcr_u32 wr_hash(const mock *m)
{
    vcr_u32 h = 2166136261u, i, k, v[4];
    for (i = 0; i < m->nw; i++) {
        v[0] = (vcr_u32)(unsigned char)m->w[i].kind;
        v[1] = m->w[i].chip;
        v[2] = m->w[i].off;
        v[3] = m->w[i].val;
        for (k = 0; k < 16; k++) {
            h ^= (v[k >> 2] >> ((k & 3) * 8)) & 0xffu;
            h *= 16777619u;
        }
    }
    return h;
}

typedef struct {
    const char *name;
    int board;                      /* chips on the mocked board */
    vcr_u32 n, sli, aa, high, analog, nlines, bpp, col, dbeg, dend;
    int then_disable;               /* pin Glide's disable after the enable instead */
    int rc;
    unsigned nw;
    vcr_u32 hash;
} seq_case;

static const seq_case k_seq[] = {
    /* cfg 2 and cfg 5 on the V5 6000 both arrive as 4-way analog SLI */
    { "cfg 5: 4-way analog SLI, 8-line bands, 16 bpp", 4, 4, 1, 0, 0, 1, 8, 16, 0, 0, 0, 0,
      VCR_SLI_W_NOCLOCK, 438, 0x0d69ff86u },
    { "cfg 5: 4-way analog SLI, 32-line bands, 16 bpp", 4, 4, 1, 0, 0, 1, 32, 16, 0, 0, 0, 0,
      VCR_SLI_W_NOCLOCK, 438, 0x873351e6u },
    { "cfg 5: 4-way analog SLI, 8-line bands, 32 bpp", 4, 4, 1, 0, 0, 1, 8, 32, 0, 0, 0, 0,
      VCR_SLI_W_NOCLOCK, 438, 0x0d69ff86u },
    /* the close of a cfg 2/5 session: Glide's SLI_AA_REQUEST disable, garbage
     * in all but dwChips. (cfg 0 never sends an SLI_AA_REQUEST at all: its
     * close is 24 PCI_OP zero writes - cfg0_close_pci_op_zeros_reach_the_chips_
     * as_before; this row was mislabelled "cfg 0 side" until 2026-09-27.) */
    { "cfg 2/5 close: Glide's disable after a 4-way SLI session", 4, 4, 1, 0, 0, 1, 8, 16, 0, 0, 0, 1,
      VCR_SLI_OK, 42, 0x66c316b2u },
    /* a V5 5500 */
    { "2-way digital SLI (2-chip board)", 2, 2, 1, 0, 0, 0, 16, 16, 0, 0, 0, 0,
      VCR_SLI_OK, 163, 0x0a263f4du },
    { "2-way analog SLI (2-chip board)", 2, 2, 1, 0, 0, 1, 16, 16, 0, 0, 0, 0,
      VCR_SLI_OK, 163, 0x3f925814u },
    /* the AA tuples that keep a video-mux branch: still accepted by the pure
     * sequence (the kernel's Diag\SliAA gate stops them first by default) */
    { "cfg 3: two 2-way analog SLI units, 2-sample AA", 4, 4, 1, 1, 0, 1, 8, 16,
      0, 0x01000000u, 0x01180000u, 0, VCR_SLI_W_NOCLOCK, 447, 0x12150b60u },
    { "cfg 7: 4 chips, no SLI, 4-sample analog AA", 4, 4, 0, 1, 1, 1, 8, 16,
      0, 0x01000000u, 0x01180000u, 0, VCR_SLI_W_NOCLOCK, 444, 0xf3ccf17eu },
    /* the one pinned request with a real AA base: 097b1f7's own sequence,
     * D:871's << 4 and all (cfgAALfbCtrl 0x8f000000). cebdf4f re-pinned it to
     * 0x8b5cc0ce by making the byte-address base unconditional; that base is
     * the vendor recipe's alone since (header difference 13), so the default
     * is 097b1f7's again. */
    { "cfg 8: 4 chips, no SLI, 8-sample analog AA", 4, 4, 0, 1, 2, 1, 8, 16,
      0x00b00000u, 0x01000000u, 0x01180000u, 0, VCR_SLI_W_NOCLOCK, 444, 0xebcc4582u },
};

static void run_seq_ex(mock *m, const seq_case *s, vcr_u32 flags, int *rc)
{
    vcr_sli_io io;
    vcr_sli_aa_req r = req(s->n, s->sli, s->aa, s->high, s->analog, s->nlines, s->bpp);
    r.MemInfo.dwaaSecondaryColorBufBegin = s->col;
    r.MemInfo.dwaaSecondaryDepthBufBegin = s->dbeg;
    r.MemInfo.dwaaSecondaryDepthBufEnd = s->dend;
    r.MemInfo.dwTileMark = 0x01b7e000u;
    mapped(m, &io, s->board);
    if (s->then_disable) {
        vcr_sli_aa_req off;
        CHECK(vcr_sli_set(&io, &r) >= 0, "enable before the disable failed");
        memset(&off, 0xa5, sizeof off);
        off.ChipInfo.dwChips = s->n;
        off.ChipInfo.dwsliEn = 0;
        off.ChipInfo.dwaaEn = 0;
        r = off;
    }
    m->nw = 0;
    m->unlogged = 0;
    *rc = flags ? vcr_sli_set_ex(&io, &r, flags) : vcr_sli_set(&io, &r);
}

static void run_seq(mock *m, const seq_case *s, int *rc)
{
    run_seq_ex(m, s, 0, rc);
}

TEST(accepted_requests_write_exactly_what_they_wrote_before_the_safety_net) {
    mock *m = &M;
    unsigned i;
    int rc;
    for (i = 0; i < sizeof k_seq / sizeof k_seq[0]; i++) {
        const seq_case *s = &k_seq[i];
        run_seq(m, s, &rc);
        if (rc != s->rc || m->nw != s->nw || wr_hash(m) != s->hash) {
            munit_fails++;
            fprintf(stderr, "    FAIL %s: rc %d nw %u hash 0x%08x, pinned rc %d nw %u hash 0x%08x\n",
                    s->name, rc, m->nw, wr_hash(m), s->rc, s->nw, s->hash);
        }
        CHECK(!(rc > 0 && (rc & VCR_SLI_W_NOMUX)), "an accepted request reached NOMUX");
        no_bus_faults(m);
    }
}

/* ---- the AA safety net (2026-09-27) ------------------------------------------------
 * cfg 1 on the V5 6000: Glide lays its buffers out for four chips and only then
 * forces one, so the kernel receives {4 chips, SLI off, AA on, 2-sample,
 * analog}. No video-mux branch exists for that shape anywhere; the port wrote
 * snoop, swap sync, pciInit0 and the AA apertures into all four chips, found
 * no mux, and answered success (W_NOMUX). .124 froze inside Glide's open right
 * after SET_DONE (boot #18, evidence/glidelab/postmortem_20260927). */
#define CFG1_OLD_RC     (VCR_SLI_W_NOCLOCK | VCR_SLI_W_NOMUX)   /* 097b1f7: "done" */
#define CFG1_OLD_WRITES 430u                                    /* 097b1f7, this mock */

static int last_refusal(const mock *m, vcr_u32 *reason, vcr_u32 *val)
{
    unsigned i;
    for (i = m->nl; i-- > 0;)
        if (m->l[i].step == VCR_SLI_S_REFUSED) {
            *reason = m->l[i].reg;
            *val = m->l[i].val;
            return 1;
        }
    return 0;
}

TEST(cfg1_as_glide_sends_it_is_refused_with_nothing_on_the_bus) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_u32 analog, reason = 0, val = 0;
    unsigned i;
    for (analog = 0; analog < 2; analog++) {
        vcr_sli_aa_req r = req(4, 0, 1, 0, analog, 8, 16);
        int rc;
        r.MemInfo.dwaaSecondaryDepthBufBegin = 0x01000000u;
        r.MemInfo.dwaaSecondaryDepthBufEnd = 0x01180000u;
        mapped(m, &io, 4);
        m->nw = 0;
        m->nl = 0;
        rc = vcr_sli_set(&io, &r);
        CHECK_EQ_I(rc, VCR_SLI_EINVAL);
        CHECK(rc != CFG1_OLD_RC, "cfg 1 answered success again");
        CHECK_EQ_U(m->nw, 0);                           /* was CFG1_OLD_WRITES */
        CHECK(m->nw != CFG1_OLD_WRITES, "cfg 1 programmed the board again");
        CHECK(last_refusal(m, &reason, &val), "cfg 1 refusal not logged");
        CHECK_EQ_U(reason, VCR_SLI_R_COMBO);
        CHECK_EQ_U(val, 0x40100u | analog);             /* {4,0,1,0,analog} */
        /* refused before ANY step: not even SET_BEGIN or the clock hook */
        for (i = 0; i < m->nl; i++)
            CHECK(m->l[i].step == VCR_SLI_S_REFUSED, "a step was logged before the refusal");
        CHECK(!has_step(m, VCR_SLI_S_NOMUX), "NOMUX reached");
    }
    no_bus_faults(m);
}

/* Does video_mux() have a branch for this shape? Asked of the sequence itself,
 * on a scratch mock, so the predicate cannot drift from the branch table. */
static int mux_branch_exists(vcr_u32 n, vcr_u32 sli, vcr_u32 aa, vcr_u32 high, vcr_u32 analog)
{
    static mock scratch;
    vcr_sli_io io;
    sli_p p;
    seed(&scratch, 4);
    io = mkio(&scratch);
    memset(&p, 0, sizeof p);
    p.n = n;
    p.sli = sli;
    p.aa = aa;
    p.high = aa ? high : 0;
    p.analog = analog;
    p.nlines = 32;
    p.lb = log2_lines(32);
    return video_mux(&io, &p, 0, 0) != VCR_SLI_W_NOMUX;
}

TEST(the_combination_predicate_is_the_video_mux_branch_table) {
    static const vcr_u32 ns[3] = { 1, 2, 4 };
    mock *m = &M;
    vcr_u32 ni, sli, aa, high, analog, accepted = 0, refused = 0;
    for (ni = 0; ni < 3; ni++)
        for (sli = 0; sli < 2; sli++)
            for (aa = 0; aa < 2; aa++)
                for (high = 0; high < 3; high++)
                    for (analog = 0; analog < 2; analog++) {
                        vcr_u32 n = ns[ni];
                        int ok, want, rc;
                        vcr_sli_io io;
                        vcr_sli_aa_req r;
                        if (!sli && !aa) {
                            CHECK(!vcr_sli_combo_ok(n, sli, aa, high, analog),
                                  "a disable counted as a combination");
                            continue;
                        }
                        ok = vcr_sli_combo_ok(n, sli, aa, high, analog);
                        /* the branch table, plus the input checks a vendor
                         * miniport applies: one chip = no SLI, 2-sample only;
                         * two chips never SLI + 4-sample; 8-sample is 4 chips,
                         * no SLI, analog. The last one is not academic:
                         * video_mux's 2-chip 4-sample branch tests `high` as
                         * a boolean, so {2,0,1,2,x} - 8-sample on two chips -
                         * used to be programmed as 4-sample AA. */
                        want = mux_branch_exists(n, sli, aa, high, analog) &&
                               !(n == 1 && (sli || (aa && high))) &&
                               !(n == 2 && sli && aa && high) &&
                               !(aa && high == 2 && !(n == 4 && !sli && analog));
                        if (ok != want) {
                            munit_fails++;
                            fprintf(stderr, "    FAIL shape {%u,%u,%u,%u,%u}: predicate %d, "
                                    "mux table %d\n", n, sli, aa, high, analog, ok, want);
                        }
                        /* and through the entry point: accepted shapes run
                         * (never to NOMUX), refused ones touch nothing */
                        r = req(n, sli, aa, high, analog, 32, 16);
                        mapped(m, &io, 4);
                        m->nw = 0;
                        rc = vcr_sli_set(&io, &r);
                        if (ok) {
                            accepted++;
                            CHECK(rc >= 0, "an accepted shape was refused");
                            CHECK(!(rc & VCR_SLI_W_NOMUX), "an accepted shape reached NOMUX");
                        } else {
                            refused++;
                            CHECK(rc < 0, "a shape with no mux was programmed");
                            CHECK_EQ_U(m->nw, 0);
                        }
                    }
    /* counted per iteration (an SLI-only shape is walked once per unused
     * sample value), so any edit to the table shows here: 2 one-chip, 12
     * two-chip, 12 four-chip accepted; 28 refused, cfg 1's two among them */
    CHECK_EQ_U(accepted, 26);
    CHECK_EQ_U(refused, 28);
    /* the two the vendor check adds over the branch table: they had a branch */
    CHECK(mux_branch_exists(2, 0, 1, 2, 0) && mux_branch_exists(2, 0, 1, 2, 1),
          "the 2-chip 8-sample shapes lost their (4-sample) branch - recount");
    CHECK(!vcr_sli_combo_ok(2, 0, 1, 2, 0) && !vcr_sli_combo_ok(2, 0, 1, 2, 1),
          "8-sample AA accepted on two chips");
}

TEST(sample_count_is_ignored_without_aa_and_out_of_range_with_it) {
    /* Glide leaves dwaaSampleHigh as it likes when AA is off */
    CHECK(vcr_sli_combo_ok(4, 1, 0, 7, 1), "SLI-only refused over an unused field");
    CHECK(!vcr_sli_combo_ok(4, 0, 1, 3, 1), "16-sample AA accepted");
    CHECK(!vcr_sli_combo_ok(4, 0, 1, 0x10, 1), "a sample count that packs to 0 accepted");
    CHECK(!vcr_sli_combo_ok(3, 1, 0, 0, 1), "three chips accepted");
    CHECK(!vcr_sli_combo_ok(0, 0, 1, 0, 0), "zero chips accepted");
    /* booleans are booleans */
    CHECK(vcr_sli_combo_ok(4, 5, 0, 0, 9), "non-0/1 flags misread");
    CHECK_EQ_U(VCR_SLI_TUPLE(4, 0, 1, 0, 1), 0x40101u);
    CHECK_EQ_U(VCR_SLI_TUPLE(4, 1, 1, 0x10, 1), 0x411f1u);     /* saturates, never wraps */
}

TEST(the_kill_switch_refuses_every_aa_request_and_nothing_else) {
    vcr_sli_aa_req r;
    /* SLI only - the proven cfg 2/5 - with the switch off and on */
    r = req(4, 1, 0, 0, 1, 8, 16);
    CHECK_EQ_I(vcr_sli_policy(&r, 0), 0);
    CHECK_EQ_I(vcr_sli_policy(&r, 1), 0);
    r = req(2, 1, 0, 0, 0, 16, 16);
    CHECK_EQ_I(vcr_sli_policy(&r, 0), 0);
    /* every disable, garbage and all */
    memset(&r, 0xa5, sizeof r);
    r.ChipInfo.dwChips = 4;
    r.ChipInfo.dwsliEn = 0;
    r.ChipInfo.dwaaEn = 0;
    CHECK_EQ_I(vcr_sli_policy(&r, 0), 0);
    /* every AA shape, supported or not, is refused while Diag\SliAA = 0 ... */
    r = req(4, 1, 1, 0, 1, 8, 16);                              /* cfg 3 */
    CHECK_EQ_I(vcr_sli_policy(&r, 0), VCR_SLI_R_AA_OFF);
    CHECK_EQ_I(vcr_sli_policy(&r, 1), 0);
    r = req(4, 0, 1, 1, 1, 8, 16);                              /* cfg 7 */
    CHECK_EQ_I(vcr_sli_policy(&r, 0), VCR_SLI_R_AA_OFF);
    CHECK_EQ_I(vcr_sli_policy(&r, 1), 0);
    r = req(4, 0, 1, 0, 1, 8, 16);                              /* cfg 1 as sent */
    CHECK_EQ_I(vcr_sli_policy(&r, 0), VCR_SLI_R_AA_OFF);
    /* ... and an armed switch still cannot pass a shape with no mux */
    CHECK_EQ_I(vcr_sli_policy(&r, 1), VCR_SLI_R_COMBO);
    r = req(1, 0, 1, 0, 0, 8, 16);                              /* single-chip AA */
    CHECK_EQ_I(vcr_sli_policy(&r, 0), VCR_SLI_R_AA_OFF);
    CHECK_EQ_U(vcr_sli_req_tuple(&r), 0x10100u);
    /* the kernel's answer for each */
    CHECK_EQ_I(refuse(&(vcr_sli_io){ 0 }, VCR_SLI_R_AA_OFF, 0), VCR_SLI_EDENIED);
    CHECK_EQ_I(refuse(&(vcr_sli_io){ 0 }, VCR_SLI_R_COMBO, 0), VCR_SLI_EINVAL);

    /* every shape: with the switch at its default, NO request that enables AA
     * gets past the policy (so VcrSliRequest never reaches the sequence and
     * nothing is written), and every SLI-only request is judged exactly as
     * the sequence itself would judge it */
    {
        static const vcr_u32 ns[4] = { 1, 2, 3, 4 };
        vcr_u32 ni, sli, aa, high, analog;
        for (ni = 0; ni < 4; ni++)
            for (sli = 0; sli < 2; sli++)
                for (aa = 0; aa < 2; aa++)
                    for (high = 0; high < 4; high++)
                        for (analog = 0; analog < 2; analog++) {
                            r = req(ns[ni], sli, aa, high, analog, 32, 16);
                            if (aa)
                                CHECK_EQ_I(vcr_sli_policy(&r, 0), VCR_SLI_R_AA_OFF);
                            else if (sli)
                                CHECK_EQ_I(vcr_sli_policy(&r, 0),
                                           vcr_sli_combo_ok(ns[ni], 1, 0, high, analog)
                                               ? 0 : VCR_SLI_R_COMBO);
                            else
                                CHECK_EQ_I(vcr_sli_policy(&r, 0), 0);
                        }
    }
}

/* ---- what survives the power cycle ------------------------------------------------ */

TEST(the_persisted_phase_keeps_the_warn_mask_the_clock_result_and_the_refusal) {
    /* SET_DONE of a 4-chip enable with NOCLOCK|NOMUX: the phase used to keep
     * chip << 24 | reg - 0x04000000, the warn mask gone (boot #18's read-back
     * could not say whether NOMUX was set) */
    CHECK_EQ_U(vcr_sli_phase_b(VCR_SLI_S_SET_DONE, 4, 0, 6), 0x04800006u);
    CHECK(vcr_sli_phase_b(VCR_SLI_S_SET_DONE, 4, 0, 6) != 0x04000000u, "warn mask dropped");
    CHECK_EQ_U(vcr_sli_phase_b(VCR_SLI_S_SET_DONE, 4, 0, 0), 0x04800000u);   /* "warn 0" != old */
    CHECK_EQ_U(vcr_sli_phase_b(VCR_SLI_S_OFF_DONE, 4, 0, 1), 0x04800001u);
    /* the clock hook's result, 23-bit signed */
    CHECK_EQ_U(vcr_sli_phase_b(VCR_SLI_S_CLOCK_6K, 0, 0, (vcr_u32)VCR_SLI_ENOTIMPL), 0x00fffffdu);
    CHECK_EQ_U(vcr_sli_phase_b(VCR_SLI_S_NOMUX, 2, 0, 0x6), 0x02800006u);
    /* a refusal: the reason and the request's shape */
    CHECK_EQ_U(vcr_sli_phase_b(VCR_SLI_S_REFUSED, 0, VCR_SLI_R_COMBO, 0x40101u), 0x09840101u);
    CHECK_EQ_U(vcr_sli_phase_b(VCR_SLI_S_REFUSED, 0, VCR_SLI_R_AA_OFF, 0x41101u), 0x0a841101u);
    /* every other step: unchanged, chip << 24 | register */
    CHECK_EQ_U(vcr_sli_phase_b(VCR_SLI_S_PCIINIT0, 3, VCR_R_PCIINIT0, 0x303), 0x03000004u);
    CHECK_EQ_U(vcr_sli_phase_b(VCR_SLI_S_SET_BEGIN, 4, 6, 8), 0x04000006u);

    /* which steps persist: the milestones, now with NOMUX; every step with
     * Diag\SliPersistAll */
    CHECK(vcr_sli_step_persists(VCR_SLI_S_NOMUX, 0), "NOMUX not persisted (it used to be dropped)");
    CHECK(vcr_sli_step_persists(VCR_SLI_S_SET_DONE, 0), "SET_DONE not persisted");
    CHECK(vcr_sli_step_persists(VCR_SLI_S_REFUSED, 0), "a refusal not persisted");
    CHECK(vcr_sli_step_persists(VCR_SLI_S_CLOCK_6K, 0), "CLOCK_6K not persisted");
    CHECK(!vcr_sli_step_persists(VCR_SLI_S_MODE_VGA, 0), "every VGA write persisted by default");
    CHECK(!vcr_sli_step_persists(VCR_SLI_S_AALFBCTRL, 0), "every config write persisted by default");
    CHECK(vcr_sli_step_persists(VCR_SLI_S_MODE_VGA, 1), "SliPersistAll left a step out");
    CHECK(vcr_sli_step_persists(VCR_SLI_S_AALFBCTRL, 1), "SliPersistAll left a step out");
}

TEST(glide_may_not_write_the_sli_aa_registers_behind_the_kernel) {
    vcr_sli_poke_memo memo;
    vcr_u32 off;
    /* the old guard refused only the standard header (< 0x40) */
    for (off = 0x40; off < 0x100; off += 4) {
        int owned = off == 0x40 || off == 0x48 || (off >= 0x80 && off <= 0xac);
        CHECK_EQ_I(vcr_sli_cfg_owned(off, 4), owned);
    }
    CHECK(!vcr_sli_cfg_owned(0x44, 4) && !vcr_sli_cfg_owned(0x4c, 4), "neighbours refused");
    CHECK(!vcr_sli_cfg_owned(0x3c, 4) && !vcr_sli_cfg_owned(0xb0, 4), "outside refused");
    /* a narrow or straddling write cannot slip a byte in */
    CHECK(vcr_sli_cfg_owned(0x41, 1), "byte write to cfgInitEnable passed");
    CHECK(vcr_sli_cfg_owned(0x4a, 2), "word write to cfgPciDecode passed");
    CHECK(vcr_sli_cfg_owned(0x7e, 4), "straddling write into cfgVideoCtrl0 passed");
    CHECK(vcr_sli_cfg_owned(0xaf, 1), "last byte of cfgSliAAMisc passed");
    CHECK(!vcr_sli_cfg_owned(0x7c, 4), "0x7c refused");
    CHECK(!vcr_sli_cfg_owned(0x80, 0), "empty write refused");

    /* persisted once per (chip, register, value): Glide's per-close zeros
     * cannot flush the phase history */
    memset(&memo, 0, sizeof memo);
    CHECK_EQ_I(vcr_sli_poke_first(&memo, 2, 0x80, 4, 0), 1);
    CHECK_EQ_I(vcr_sli_poke_first(&memo, 2, 0x80, 4, 0), 0);
    CHECK_EQ_I(vcr_sli_poke_first(&memo, 2, 0x80, 4, 0x1009), 1);   /* EN|LOCALMUX|DIV2 */
    CHECK_EQ_I(vcr_sli_poke_first(&memo, 2, 0x80, 4, 0x1009), 0);
    CHECK_EQ_I(vcr_sli_poke_first(&memo, 2, 0x80, 4, 0), 1);         /* changed back */
    CHECK_EQ_I(vcr_sli_poke_first(&memo, 3, 0x80, 4, 0), 1);         /* another chip */
    CHECK_EQ_I(vcr_sli_poke_first(&memo, 2, 0x94, 4, 0), 1);         /* another register */
    CHECK_EQ_I(vcr_sli_poke_first(&memo, 2, 0xac, 4, 0), 1);
    CHECK_EQ_I(vcr_sli_poke_first(&memo, 2, 0xac, 4, 0), 0);
    CHECK_EQ_I(vcr_sli_poke_first(&memo, 7, 0x80, 4, 0), 1);         /* no slot: persist */
    CHECK_EQ_I(vcr_sli_poke_first(NULL, 0, 0x80, 4, 0), 1);
}

/* ---- PCI_OP writes, as Glide's NT build really sends them (review 2026-09-27) --------
 * pci_op (vcrmp.c) over the mock: the flags it builds (SliAA asked only for a
 * write that needs it), vcr_sli_poke_policy, the phase memo. poke_old() is the
 * guard 412b03c shipped - every write touching 0x40/0x48/0x80-0xAF refused
 * unless AllowPoke - kept to show what it did to the same traffic. */
typedef struct {
    unsigned passed, refused, phases;
    int last_why;
    vcr_sli_poke_memo memo;
} poke_run;

static void poke_reset(poke_run *pr)
{
    memset(pr, 0, sizeof *pr);
}

static void poke_land(mock *m, vcr_u32 chip, vcr_u32 off, vcr_u32 val)
{
    if (chip < NCH && off >= 0x40)
        m->cfg[chip][CFGI(off)] = val;
}

static int poke_new(mock *m, poke_run *pr, vcr_u32 chip, vcr_u32 off, vcr_u32 val, int allow,
                    int sliaa, vcr_u32 live)
{
    vcr_u32 flags = allow ? VCR_POKE_F_ALLOW : 0;
    int why;
    if (vcr_sli_poke_enables_aa(off, 4, val) && sliaa)
        flags |= VCR_POKE_F_AA;
    why = vcr_sli_poke_policy(chip, off, 4, val, flags, live);
    pr->last_why = why;
    if (why) {
        pr->refused++;
        if (why != VCR_POKE_R_HEADER && vcr_sli_poke_first(&pr->memo, chip, off, 4, val))
            pr->phases++;
        return 0;
    }
    /* the kernel's pci_op: an idle slave keeps its syncs tristated */
    poke_land(m, chip, off, vcr_sli_poke_adjust(chip, off, 4, val, live));
    pr->passed++;
    return 1;
}

static int poke_old(mock *m, poke_run *pr, vcr_u32 chip, vcr_u32 off, vcr_u32 val, int allow)
{
    if ((off < 0x40 || vcr_sli_cfg_owned(off, 4)) && !allow) {
        pr->refused++;
        if (off >= 0x40 && vcr_sli_poke_first(&pr->memo, chip, off, 4, val))
            pr->phases++;
        return 0;
    }
    poke_land(m, chip, off, val);
    pr->passed++;
    return 1;
}

/* hwcRestoreVideo (minihwc.c), a Napalm close that was not an SLI / multi-chip
 * AA session - every cfg 0 close: per chip Glide counts, 0 into these, in
 * this order (cfgVideoCtrl2 twice, as the source has it) */
static const vcr_u32 k_close_offs[6] = {
    VCR_CFG_SLILFBCTRL, VCR_CFG_AADEPTHBUFAPERTURE, VCR_CFG_AALFBCTRL,
    VCR_CFG_VIDEOCTRL0, VCR_CFG_VIDEOCTRL2, VCR_CFG_VIDEOCTRL2
};

/* after a cfg 5 session and the kernel's disable: what a cfg 0 run meets */
static void after_sli_session(mock *m, vcr_sli_io *io)
{
    vcr_sli_aa_req r = req(4, 1, 0, 0, 1, 8, 16), off;
    mapped(m, io, 4);
    CHECK(vcr_sli_set(io, &r) >= 0, "cfg 5 enable");
    memset(&off, 0, sizeof off);
    off.ChipInfo.dwChips = 4;
    CHECK(vcr_sli_set(io, &off) >= 0, "cfg 5 disable");
}

TEST(an_idle_slave_keeps_its_syncs_tristated_whatever_a_pci_op_writes) {
    const vcr_u32 keep = VCR_VC0_DAC_HSYNC_TRISTATE | VCR_VC0_DAC_VSYNC_TRISTATE;
    vcr_u32 c;
    CHECK_EQ_U(keep, 0x03000000u);
    for (c = 1; c < 4; c++) {
        /* idle (no live session): the tristate bits forced on, the rest kept */
        CHECK_EQ_U(vcr_sli_poke_adjust(c, VCR_CFG_VIDEOCTRL0, 4, 0, 0), keep);
        CHECK_EQ_U(vcr_sli_poke_adjust(c, VCR_CFG_VIDEOCTRL0, 4, 0x00000105u, 0), keep | 0x105u);
        /* a byte / word write that covers bits 24-25 */
        CHECK_EQ_U(vcr_sli_poke_adjust(c, VCR_CFG_VIDEOCTRL0 + 3, 1, 0, 0), 0x03u);
        CHECK_EQ_U(vcr_sli_poke_adjust(c, VCR_CFG_VIDEOCTRL0 + 2, 2, 0, 0), 0x0300u);
        /* ... and one that does not */
        CHECK_EQ_U(vcr_sli_poke_adjust(c, VCR_CFG_VIDEOCTRL0, 2, 0, 0), 0);
        CHECK_EQ_U(vcr_sli_poke_adjust(c, VCR_CFG_VIDEOCTRL0, 1, 0, 0), 0);
        /* a slave IN a live session is Glide's to program: unchanged */
        CHECK_EQ_U(vcr_sli_poke_adjust(c, VCR_CFG_VIDEOCTRL0, 4, 0, 4), 0);
        /* any other register: unchanged */
        CHECK_EQ_U(vcr_sli_poke_adjust(c, VCR_CFG_VIDEOCTRL2, 4, 0, 0), 0);
        CHECK_EQ_U(vcr_sli_poke_adjust(c, VCR_CFG_AALFBCTRL, 4, 0, 0), 0);
    }
    /* the master drives the monitor: never touched */
    CHECK_EQ_U(vcr_sli_poke_adjust(0, VCR_CFG_VIDEOCTRL0, 4, 0, 0), 0);
    /* out of bounds: unchanged (the policy refuses it anyway) */
    CHECK_EQ_U(vcr_sli_poke_adjust(1, 0x81, 4, 0, 0), 0);
}

TEST(cfg0_close_pci_op_zeros_reach_the_chips_as_before) {
    mock *m = &M;
    vcr_sli_io io;
    poke_run pr;
    vcr_u32 c, i;
    /* the state the kernel's disable leaves: the slaves' syncs tristated */
    after_sli_session(m, &io);
    for (c = 1; c < 4; c++)
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL0), 0x03000000u);

    /* NEW (the defaults: AllowPoke 0, SliAA 0, no live session): all 24 go
     * through - no refusal, no flushed phase - but an idle slave's
     * cfgVideoCtrl0 lands with its syncs still tristated (2026-09-27: the
     * zeros un-tristated them and .124's monitor lost sync) */
    poke_reset(&pr);
    for (c = 0; c < 4; c++)
        for (i = 0; i < 6; i++)
            CHECK(poke_new(m, &pr, c, k_close_offs[i], 0, 0, 0, 0), "a close zero refused");
    CHECK_EQ_U(pr.passed, 24);
    CHECK_EQ_U(pr.refused, 0);
    CHECK_EQ_U(pr.phases, 0);
    for (c = 0; c < 4; c++) {                   /* the post-close state */
        /* master 0 as ever; slaves tristated - 097b1f7 left them 0, driving */
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL0), c ? 0x03000000u : 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL2), 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_AALFBCTRL), 0);
    }
    /* the same zeros are the same answer with the switches up, too */
    for (c = 0; c < 4; c++)
        for (i = 0; i < 6; i++)
            CHECK_EQ_I(vcr_sli_poke_policy(c, k_close_offs[i], 4, 0,
                                           VCR_POKE_F_ALLOW | VCR_POKE_F_AA, 0), 0);

    /* OLD (412b03c): all 24 refused, the slaves left tristated, and 20 flushed
     * phases (5 registers x 4 chips; the second cfgVideoCtrl2 is a memo hit)
     * on the first close of every boot - ~0.3-0.6 s of ZwFlushKey inside
     * Glide's close, 20 of the 64 history slots, LastPhase SLI_POKE_REFUSED */
    after_sli_session(m, &io);
    poke_reset(&pr);
    for (c = 0; c < 4; c++)
        for (i = 0; i < 6; i++)
            CHECK(!poke_old(m, &pr, c, k_close_offs[i], 0, 0), "the old guard let a zero through");
    CHECK_EQ_U(pr.refused, 24);
    CHECK_EQ_U(pr.phases, 20);
    for (c = 1; c < 4; c++)
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL0), 0x03000000u);
    /* a second close in the same boot: the memo keeps the old guard quiet */
    pr.refused = pr.phases = 0;
    for (c = 0; c < 4; c++)
        for (i = 0; i < 6; i++)
            poke_old(m, &pr, c, k_close_offs[i], 0, 0);
    CHECK_EQ_U(pr.phases, 0);
}

/* minihwc.c, IS_NAPALM && h3pixelSample == 2 and no SLI_AA_REQUEST: per chip
 * (every chip Glide counts upstream; the chips it drives with SLIAA-GUARD) */
#define SCAA_AALFB   (VCR_AALFB_CPU_WRITE_EN | VCR_AALFB_DISPATCH_WRITE_EN | VCR_AALFB_READ_EN | \
                      0x00c00000u /* colBuffStart1[0] */ | VCR_AALFB_FMT_16BPP)
#define SCAA_DEPTH   (0x1000u | (0x1180u << 16))
#define SCAA_VC0     (VCR_VC0_ENHANCED_VIDEO_EN | VCR_VC0_LOCALMUX_DESKTOP_PLUS_OVERLAY | VCR_VC0_DIVIDE_BY_2)
static const struct { vcr_u32 off, val; int aa; } k_scaa[6] = {
    { VCR_CFG_SLILFBCTRL, 0, 0 },
    { VCR_CFG_AALFBCTRL, SCAA_AALFB, 1 },
    { VCR_CFG_AADEPTHBUFAPERTURE, SCAA_DEPTH, 1 },
    { VCR_CFG_VIDEOCTRL0, SCAA_VC0, 1 },
    { VCR_CFG_VIDEOCTRL1, 0, 0 },
    { VCR_CFG_VIDEOCTRL2, 0, 0 },
};

static void single_chip_aa(mock *m, poke_run *pr, int allow, int sliaa, vcr_u32 live)
{
    vcr_u32 c, i;
    poke_reset(pr);
    for (c = 0; c < 4; c++)
        for (i = 0; i < 6; i++)
            poke_new(m, pr, c, k_scaa[i].off, k_scaa[i].val, allow, sliaa, live);
}

TEST(single_chip_aa_pci_op_writes_need_diag_sliaa_even_with_allow_poke) {
    mock *m = &M;
    vcr_sli_io io;
    poke_run pr;
    vcr_u32 c, i;
    CHECK_EQ_U(SCAA_VC0, 0x1009u);
    for (i = 0; i < 6; i++)
        CHECK_EQ_I(vcr_sli_poke_enables_aa(k_scaa[i].off, 4, k_scaa[i].val), k_scaa[i].aa);

    /* the defaults: every AA value refused (AA_OFF), on every chip; the zeros
     * (the same as the close's) land. No chip gets the video merge. */
    mapped(m, &io, 4);
    single_chip_aa(m, &pr, 0, 0, 0);
    CHECK_EQ_U(pr.refused, 12);
    CHECK_EQ_U(pr.passed, 12);
    CHECK_EQ_U(pr.phases, 12);                  /* each (chip, register, value) once */
    for (c = 0; c < 4; c++) {
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL0) & VCR_VC0_ENHANCED_VIDEO_EN, 0);
        CHECK_EQ_U(CFG(m, c, VCR_CFG_AALFBCTRL), 0);
    }
    CHECK_EQ_I(vcr_sli_poke_policy(0, VCR_CFG_VIDEOCTRL0, 4, SCAA_VC0, 0, 0), VCR_POKE_R_AA_OFF);
    /* AllowPoke does NOT open it: the kill switch is Diag\SliAA */
    mapped(m, &io, 4);
    single_chip_aa(m, &pr, 1, 0, 0);
    CHECK_EQ_U(pr.refused, 12);
    CHECK_EQ_I(vcr_sli_poke_policy(0, VCR_CFG_VIDEOCTRL0, 4, SCAA_VC0, VCR_POKE_F_ALLOW, 0),
               VCR_POKE_R_AA_OFF);
    /* OLD (412b03c) with AllowPoke = 1: all 24 went through - AA programmed
     * on four chips with Diag\SliAA = 0, the slaves' syncs un-tristated */
    {
        poke_run po;
        mapped(m, &io, 4);
        poke_reset(&po);
        for (c = 0; c < 4; c++)
            for (i = 0; i < 6; i++)
                poke_old(m, &po, c, k_scaa[i].off, k_scaa[i].val, 1);
        CHECK_EQ_U(po.passed, 24);
        for (c = 0; c < 4; c++)
            CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL0), SCAA_VC0);
    }
    /* Diag\SliAA = 1, AllowPoke 0: the master (the chip single-chip AA
     * drives) is programmed; the slaves - outside any kernel session - are not */
    mapped(m, &io, 4);
    single_chip_aa(m, &pr, 0, 1, 0);
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_VIDEOCTRL0), SCAA_VC0);
    CHECK_EQ_U(CFG(m, 0, VCR_CFG_AALFBCTRL), SCAA_AALFB);
    for (c = 1; c < 4; c++)
        CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL0) & VCR_VC0_ENHANCED_VIDEO_EN, 0);
    CHECK_EQ_U(pr.refused, 9);                  /* 3 AA values x 3 slaves */
    CHECK_EQ_I(vcr_sli_poke_policy(2, VCR_CFG_VIDEOCTRL0, 4, SCAA_VC0, VCR_POKE_F_AA, 0),
               VCR_POKE_R_SLAVE);
    /* ... inside a live 4-chip kernel session, or with AllowPoke, they are */
    CHECK_EQ_I(vcr_sli_poke_policy(2, VCR_CFG_VIDEOCTRL0, 4, SCAA_VC0, VCR_POKE_F_AA, 4), 0);
    CHECK_EQ_I(vcr_sli_poke_policy(2, VCR_CFG_VIDEOCTRL0, 4, SCAA_VC0,
                                   VCR_POKE_F_AA | VCR_POKE_F_ALLOW, 0), 0);
    CHECK_EQ_I(vcr_sli_poke_policy(2, VCR_CFG_VIDEOCTRL0, 4, SCAA_VC0, VCR_POKE_F_AA, 2),
               VCR_POKE_R_SLAVE);               /* chip 2 is outside a 2-chip session */
    /* a byte or word write cannot slip an AA bit in */
    CHECK_EQ_I(vcr_sli_poke_enables_aa(0x80, 1, 0x09), 1);
    CHECK_EQ_I(vcr_sli_poke_enables_aa(0x81, 1, 0x10), 1);      /* DIVIDE_BY_2 */
    CHECK_EQ_I(vcr_sli_poke_enables_aa(0x83, 1, 0x03), 0);      /* the tristate bits alone */
    CHECK_EQ_I(vcr_sli_poke_enables_aa(0x96, 2, 0x0c00), 1);    /* CPU + dispatch write */
    CHECK_EQ_I(vcr_sli_poke_enables_aa(0x97, 1, 0x10), 0);      /* READ_EN alone */
    CHECK_EQ_I(vcr_sli_poke_enables_aa(0x92, 2, 0x0001), 1);    /* depth aperture end */
    CHECK_EQ_I(vcr_sli_poke_enables_aa(0x8c, 4, 0x1d070008u), 0);  /* SLI LFB control */
    CHECK_EQ_I(vcr_sli_poke_enables_aa(0xac, 4, 0x182fu), 0);
    CHECK_EQ_I(vcr_sli_poke_enables_aa(0x80, 4, 0x03000000u), 0);
}

TEST(a0_read_toggles_in_a_live_sli_session_go_through) {
    /* hwcSLIReadDisable/Enable (FX_GLIDE_A0_READ_ABORT, n-way SLI): read-
     * modify-write of READ_EN in cfgSliLfbCtrl and cfgAALfbCtrl, while the
     * kernel's cfg 5 session is live - never measured on .124 as set or unset,
     * so it must keep 097b1f7's answer either way */
    mock *m = &M;
    vcr_sli_io io;
    poke_run pr, po;
    vcr_sli_aa_req r = req(4, 1, 0, 0, 1, 8, 16);
    vcr_u32 c, sli;
    mapped(m, &io, 4);
    CHECK(vcr_sli_set(&io, &r) >= 0, "cfg 5");
    poke_reset(&pr);
    poke_reset(&po);
    for (c = 0; c < 4; c++) {
        sli = CFG(m, c, VCR_CFG_SLILFBCTRL);
        /* disable: SLI read off, AA read on; enable: back */
        CHECK(poke_new(m, &pr, c, VCR_CFG_SLILFBCTRL, sli & ~VCR_SLILFB_READ_EN, 0, 0, 4), "SLI read off");
        CHECK(poke_new(m, &pr, c, VCR_CFG_AALFBCTRL, VCR_AALFB_READ_EN, 0, 0, 4), "AA read on");
        CHECK(poke_new(m, &pr, c, VCR_CFG_SLILFBCTRL, sli, 0, 0, 4), "SLI read on");
        CHECK(poke_new(m, &pr, c, VCR_CFG_AALFBCTRL, 0, 0, 0, 4), "AA read off");
        CHECK_EQ_U(CFG(m, c, VCR_CFG_SLILFBCTRL), sli);
        /* OLD: every one refused */
        CHECK(!poke_old(m, &po, c, VCR_CFG_SLILFBCTRL, sli & ~VCR_SLILFB_READ_EN, 0), "old");
        CHECK(!poke_old(m, &po, c, VCR_CFG_AALFBCTRL, VCR_AALFB_READ_EN, 0), "old");
    }
    CHECK_EQ_U(pr.refused, 0);
    CHECK_EQ_U(po.refused, 8);
}

TEST(pci_op_offsets_outside_the_config_header_are_refused) {
    /* a slave is written by raw 0xCF8 cycles, which keep only offset bits
     * 2-7: 0x140 is the slave's cfgVideoCtrl0, 0x104 its command register */
    CHECK_EQ_U(VCR_PCI_CF8(3, 0, 1, 0x140) & 0xfc, 0x40);
    CHECK_EQ_U(VCR_PCI_CF8(3, 0, 1, 0x180) & 0xfc, 0x80);
    CHECK_EQ_U(VCR_PCI_CF8(3, 0, 1, 0x104) & 0xfc, 0x04);
    /* OLD: neither guard saw them - owned_first() said "not ours" past 0xff
     * and the header guard only looks below 0x40 */
    CHECK_EQ_I(vcr_sli_cfg_owned(0x140, 4), 0);
    CHECK_EQ_I(vcr_sli_cfg_owned(0x180, 4), 0);
    CHECK(!(0x104 < 0x40), "0x104 below the header");
    /* NEW: refused before either guard, reads too */
    CHECK_EQ_I(vcr_cfg_access_ok(0x140, 4), 0);
    CHECK_EQ_I(vcr_cfg_access_ok(0x104, 4), 0);
    CHECK_EQ_I(vcr_cfg_access_ok(0x1ac, 4), 0);
    CHECK_EQ_I(vcr_cfg_access_ok(0x100, 1), 0);
    CHECK_EQ_I(vcr_cfg_access_ok(0xfc, 4), 1);
    CHECK_EQ_I(vcr_cfg_access_ok(0xff, 1), 1);
    CHECK_EQ_I(vcr_cfg_access_ok(0xfe, 4), 0);      /* past the end */
    CHECK_EQ_I(vcr_cfg_access_ok(0x7e, 4), 0);      /* straddles into cfgVideoCtrl0 */
    CHECK_EQ_I(vcr_cfg_access_ok(0x41, 2), 0);      /* misaligned word */
    CHECK_EQ_I(vcr_cfg_access_ok(0x42, 2), 1);
    CHECK_EQ_I(vcr_cfg_access_ok(0x80, 3), 0);
    CHECK_EQ_I(vcr_cfg_access_ok(0x80, 0), 0);
    CHECK_EQ_I(vcr_sli_poke_policy(1, 0x140, 4, 0x1009u, VCR_POKE_F_ALLOW | VCR_POKE_F_AA, 4),
               VCR_POKE_R_BOUNDS);
    CHECK_EQ_I(vcr_sli_poke_policy(1, 0x104, 4, 0, VCR_POKE_F_ALLOW, 0), VCR_POKE_R_BOUNDS);
    /* the header (as ever) and the snoop/decode registers need AllowPoke */
    CHECK_EQ_I(vcr_sli_poke_policy(0, 0x04, 4, 0, 0, 0), VCR_POKE_R_HEADER);
    CHECK_EQ_I(vcr_sli_poke_policy(0, 0x04, 4, 0, VCR_POKE_F_ALLOW, 0), 0);
    CHECK_EQ_I(vcr_sli_poke_policy(1, 0x40, 4, 0, 0, 0), VCR_POKE_R_SNOOP);
    CHECK_EQ_I(vcr_sli_poke_policy(1, 0x49, 1, 0, 0, 4), VCR_POKE_R_SNOOP);
    CHECK_EQ_I(vcr_sli_poke_policy(1, 0x48, 4, 0, VCR_POKE_F_ALLOW, 0), 0);
    /* non-SLI registers above the header: as before */
    CHECK_EQ_I(vcr_sli_poke_policy(0, 0x44, 4, 0, 0, 0), 0);
    CHECK_EQ_I(vcr_sli_poke_policy(0, 0xb0, 4, 0xffffffffu, 0, 0), 0);
}

/* A Banshee or Voodoo 3 is VCR_HW_VOODOO too, and 0x80-0xAC are not its
 * SLI/AA registers. dos_mode.c trusts its caller and slaves_present() asks
 * only chips 1..n-1 - none for a 1-chip request - so `vcrctl sliaa 1 0 1 0 0`
 * with Diag\SliAA = 1 on the 86Box Voodoo 3 bed ran the VSA-100 sequence. */
#define V3_OLD_AA_WRITES     25u        /* before 2026-09-27, this mock, rc 0 */
#define V3_OLD_OFF_WRITES     9u
TEST(a_non_vsa100_master_is_refused_with_nothing_written) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_sli_aa_req r;
    vcr_u32 reason = 0, val = 0, flags;
    for (flags = 0; flags <= VCR_SLI_F_VENDOR_AA; flags++) {
        seed(m, 1);
        m->cfg[0][CFGI(0x00)] = 0x0005121au;            /* 121a:0005, a Voodoo 3 */
        io = mkio(m);
        r = req(1, 0, 1, 0, 0, 8, 16);
        r.MemInfo.dwTileMark = 0x00800000u;
        m->nw = 0;
        CHECK_EQ_I(vcr_sli_set_ex(&io, &r, flags), VCR_SLI_ENODEV);
        CHECK_EQ_U(m->nw, 0);
        CHECK(m->nw != V3_OLD_AA_WRITES, "the VSA-100 sequence ran on a Voodoo 3");
        CHECK(last_refusal(m, &reason, &val) && reason == VCR_SLI_R_NODEV && val == 0,
              "not a NODEV refusal naming the master");
    }
    /* a disable too: nothing of ours is live on such a board */
    seed(m, 1);
    m->cfg[0][CFGI(0x00)] = 0x0005121au;
    io = mkio(m);
    memset(&r, 0, sizeof r);
    r.ChipInfo.dwChips = 1;
    m->nw = 0;
    CHECK_EQ_I(vcr_sli_set(&io, &r), VCR_SLI_ENODEV);
    CHECK_EQ_U(m->nw, 0);
    CHECK(m->nw != V3_OLD_OFF_WRITES, "the disable ran on a Voodoo 3");
    /* and a VSA-100 master still runs it (the pinned sequences cover the rest) */
    mapped(m, &io, 1);
    r = req(1, 0, 1, 0, 0, 8, 16);
    CHECK(vcr_sli_set(&io, &r) >= 0, "a 1-chip VSA-100 AA request refused");
}

TEST(the_vendor_memory_refusal_is_policy_before_any_teardown) {
    /* VcrSliRequest used to ask vcr_sli_policy (AA_OFF, COMBO) and then tear a
     * live session down to make room - and only then did vcr_sli_set_ex
     * refuse the vendor recipe's unusable memory info. policy_ex asks it first. */
    vcr_sli_aa_req r = req(4, 1, 1, 0, 1, 8, 16);           /* cfg 3: base = tileMark */
    r.MemInfo.dwTileMark = 0;                               /* vcrctl without one */
    /* OLD: the policy passed it */
    CHECK_EQ_I(vcr_sli_policy(&r, 1), 0);
    /* NEW: refused before anything, with the vendor recipe - */
    CHECK_EQ_I(vcr_sli_policy_ex(&r, 1, VCR_SLI_F_VENDOR_AA), VCR_SLI_R_MEMINFO);
    /* - and only then: the default recipe never reads it, AA still needs the switch */
    CHECK_EQ_I(vcr_sli_policy_ex(&r, 1, 0), 0);
    CHECK_EQ_I(vcr_sli_policy_ex(&r, 0, VCR_SLI_F_VENDOR_AA), VCR_SLI_R_AA_OFF);
    r.MemInfo.dwTileMark = 0x01b7e000u;
    CHECK_EQ_I(vcr_sli_policy_ex(&r, 1, VCR_SLI_F_VENDOR_AA), 0);
    /* cfg 8 (the aperture) needs it; a 2-sample-per-chip SLI shape does not */
    r = req(4, 0, 1, 2, 1, 8, 16);
    CHECK_EQ_I(vcr_sli_policy_ex(&r, 1, VCR_SLI_F_VENDOR_AA), VCR_SLI_R_MEMINFO);
    r = req(4, 1, 1, 1, 1, 8, 16);
    CHECK_EQ_I(vcr_sli_policy_ex(&r, 1, VCR_SLI_F_VENDOR_AA), 0);
    /* SLI only and the disable: the flag changes nothing */
    r = req(4, 1, 0, 0, 1, 8, 16);
    CHECK_EQ_I(vcr_sli_policy_ex(&r, 0, VCR_SLI_F_VENDOR_AA), 0);
    memset(&r, 0xa5, sizeof r);
    r.ChipInfo.dwChips = 4;
    r.ChipInfo.dwsliEn = r.ChipInfo.dwaaEn = 0;
    CHECK_EQ_I(vcr_sli_policy_ex(&r, 0, VCR_SLI_F_VENDOR_AA), 0);
    /* policy_ex and the sequence's backstop agree, shape by shape */
    {
        static const vcr_u32 ns[3] = { 1, 2, 4 };
        mock *m = &M;
        vcr_sli_io io;
        vcr_u32 ni, sli, high, analog;
        for (ni = 0; ni < 3; ni++)
            for (sli = 0; sli < 2; sli++)
                for (high = 0; high < 3; high++)
                    for (analog = 0; analog < 2; analog++) {
                        int pol, rc;
                        r = req(ns[ni], sli, 1, high, analog, 8, 16);
                        pol = vcr_sli_policy_ex(&r, 1, VCR_SLI_F_VENDOR_AA);
                        if (pol == VCR_SLI_R_COMBO)
                            continue;
                        mapped(m, &io, 4);
                        rc = vcr_sli_set_ex(&io, &r, VCR_SLI_F_VENDOR_AA);
                        CHECK_EQ_I(pol == VCR_SLI_R_MEMINFO, rc == VCR_SLI_EINVAL);
                    }
    }
}

/* ---- step B (2026-09-27): the AA base, a vendor-style recipe, the read-back -------- */

/* the bus hash with the VALUES of one config register's writes blanked */
static vcr_u32 wr_hash_blank(const mock *m, vcr_u32 cfg_off)
{
    vcr_u32 h = 2166136261u, i, k, v[4];
    for (i = 0; i < m->nw; i++) {
        v[0] = (vcr_u32)(unsigned char)m->w[i].kind;
        v[1] = m->w[i].chip;
        v[2] = m->w[i].off;
        v[3] = (m->w[i].kind == 'c' && m->w[i].off == cfg_off) ? 0 : m->w[i].val;
        for (k = 0; k < 16; k++) {
            h ^= (v[k >> 2] >> ((k & 3) * 8)) & 0xffu;
            h *= 16777619u;
        }
    }
    return h;
}

/* (a) cfgAALfbCtrl's secondary base is a byte address in bits 4-25 - in the
 * VENDOR recipe. The default recipe is dos_mode.c's, D:871's << 4 included:
 * the control arm of an AA A/B, byte-identical to 097b1f7 (and reachable only
 * with Diag\SliAA = 1). cebdf4f had made the byte address unconditional,
 * which moved the default arm of every shape with a real base (critic plan
 * step 12(a) put it behind the flag; review 2026-09-27). */
#define CFG8_BLANK94_OLD   0xb25fd70eu  /* cfg 8's bus hash, 0x94 values blanked (412b03c) */
#define CFG8_AALFB_OLD     0x8f000000u  /* 0x00b00000 << 4: base 176 MB on a 32 MB chip */
#define CFG8_AALFB_NEW     0x8cb00000u  /* base 0x00b00000, CPU+dispatch write, 16 bpp, /4: chips 2/3 */
/* 2026-09-30: chips 0/1 keep AA LFB reads on, as both 3dfx miniports write
 * CFG_AA_LFB_RD_EN for every AA request and clear it on chips 2/3 only */
#define CFG8_AALFB_NEW_RD  (CFG8_AALFB_NEW | VCR_AALFB_READ_EN)   /* 0x9cb00000: chips 0/1 */

TEST(the_aa_base_is_a_byte_address_in_the_vendor_recipe_only) {
    mock *m = &M;
    vcr_sli_io io;
    unsigned i, j, n94;
    int rc;
    /* the pure field: unshifted and masked to bits 4-25 */
    CHECK_EQ_U(vcr_sli_aalfb_base(0x00b00000u), 0x00b00000u);
    CHECK_EQ_U(vcr_sli_aalfb_base(0x01b7e000u), 0x01b7e000u);
    CHECK_EQ_U(vcr_sli_aalfb_base(0x0000000fu), 0);             /* not part of the address */
    CHECK_EQ_U(vcr_sli_aalfb_base(0xffffffffu), 0x03fffff0u);   /* never reaches bit 26 */
    CHECK_EQ_U(VCR_AALFB_SECONDARY_BASE_SHIFT, 4);              /* D:871 */
    /* a base of 0 - what Glide sends for cfg 3 and cfg 7 - is written the same
     * by both formulas */
    CHECK_EQ_U(vcr_sli_aalfb_base(0), 0u << VCR_AALFB_SECONDARY_BASE_SHIFT);
    /* DEFAULT recipe: every pinned AA request writes D:871's value */
    for (i = 0; i < sizeof k_seq / sizeof k_seq[0]; i++) {
        const seq_case *s = &k_seq[i];
        if (!s->aa)
            continue;
        run_seq(m, s, &rc);
        for (j = 0, n94 = 0; j < m->nw; j++) {
            if (m->w[j].kind != 'c' || m->w[j].off != VCR_CFG_AALFBCTRL)
                continue;
            n94++;
            CHECK_EQ_U(m->w[j].val & 0x3ffffff0u & ~VCR_AALFB_CPU_WRITE_EN & ~VCR_AALFB_DISPATCH_WRITE_EN,
                       (s->col << 4) & 0x3ffffff0u & ~VCR_AALFB_CPU_WRITE_EN & ~VCR_AALFB_DISPATCH_WRITE_EN);
            if (s->col) {                       /* cfg 8 */
                CHECK_EQ_U(m->w[j].val, CFG8_AALFB_OLD);
                CHECK(m->w[j].val != CFG8_AALFB_NEW, "the default recipe writes the vendor base");
            }
        }
        CHECK(n94 >= 4, "an AA request wrote cfgAALfbCtrl on fewer than 4 chips");
        if (s->col) {
            CHECK_EQ_U(n94, 6);                 /* 4 + chips 2/3 AA reads off */
            CHECK_EQ_U(wr_hash_blank(m, VCR_CFG_AALFBCTRL), CFG8_BLANK94_OLD);
        }
    }
    /* VENDOR recipe: the same cfg 8 request writes the byte address */
    {
        vcr_sli_aa_req r = req(4, 0, 1, 2, 1, 8, 16);
        r.MemInfo.dwaaSecondaryColorBufBegin = 0x00b00000u;
        r.MemInfo.dwaaSecondaryDepthBufBegin = 0x01000000u;
        r.MemInfo.dwaaSecondaryDepthBufEnd = 0x01180000u;
        r.MemInfo.dwTileMark = 0x01b7e000u;
        mapped(m, &io, 4);
        m->nw = 0;
        CHECK_EQ_I(vcr_sli_set_ex(&io, &r, VCR_SLI_F_VENDOR_AA), VCR_SLI_W_NOCLOCK);
        for (j = 0, n94 = 0; j < m->nw; j++)
            if (m->w[j].kind == 'c' && m->w[j].off == VCR_CFG_AALFBCTRL) {
                n94++;
                /* every write carries the byte address, whatever READ_EN says */
                CHECK_EQ_U(m->w[j].val & VCR_AALFB_SECONDARY_BASE_MASK, 0x00b00000u);
            }
        CHECK_EQ_U(n94, 6);
        /* what the chips are left with: READ_EN on chips 0/1, off on 2/3 */
        CHECK_EQ_U(CFG(m, 0, VCR_CFG_AALFBCTRL), CFG8_AALFB_NEW_RD);
        CHECK_EQ_U(CFG(m, 1, VCR_CFG_AALFBCTRL), CFG8_AALFB_NEW_RD);
        CHECK_EQ_U(CFG(m, 2, VCR_CFG_AALFBCTRL), CFG8_AALFB_NEW);
        CHECK_EQ_U(CFG(m, 3, VCR_CFG_AALFBCTRL), CFG8_AALFB_NEW);
        no_bus_faults(m);
    }
}

/* ---- 2026-09-30: the AA LFB control that froze every in-game AA session -----------
 * .124, Quake II through our ICD + our h5 Glide + vcr-kmd. The default
 * (dos_mode.c) recipe leaves cfgAALfbCtrl READ_EN clear on every chip - 2x read
 * back 0x4c000000 on all four - and every in-game AA session froze the box hard
 * at a random moment (the 3dfx splash, the quit; the monitor losing sync). The
 * vendor recipe sets READ_EN on the master pair as both 3dfx miniports do: 2x
 * 0xdf8f6000, 4x 0xdf1ee000 / 0xcf1ee000 - clean. 8x froze with the vendor
 * recipe (0xce3dc000 on all four: its 2-samples-per-chip branch had no READ_EN)
 * until READ_EN reached chips 0/1 (0xde3dc000 / 0xce3dc000) - with nothing else
 * changed, it then ran clean too. READ_EN on chips 0/1 is the one value that
 * separates every freeze from every clean run. Glide's exact requests and the
 * values the chips read back ("pig:" lines, evidence/glidelab/aa_supervised_0930/,
 * aa_vendor_0930/) are pinned here, the OLD values too.
 * (Withdrawn: "AA LFB writes duplicated into Glide's command FIFO through a
 * base of 0" - the traced runs show no LFB writes in game at all.) */
static void last_cfg_write_per_chip(const mock *m, vcr_u32 off, vcr_u32 out[4])
{
    unsigned j;
    out[0] = out[1] = out[2] = out[3] = 0xdeadbeefu;
    for (j = 0; j < m->nw; j++)
        if (m->w[j].kind == 'c' && m->w[j].off == off && m->w[j].chip < 4)
            out[m->w[j].chip] = m->w[j].val;
}

static vcr_sli_aa_req glide_req_124(vcr_u32 sli, vcr_u32 high, vcr_u32 tile, vcr_u32 dbeg,
                                    vcr_u32 dend)
{
    vcr_sli_aa_req r = req(4, sli, 1, high, 1, 32, 32);     /* analog, 32-line bands, 32 bpp */
    r.MemInfo.dwTotalMemory = 0x04000000u;
    r.MemInfo.dwTileMark = tile;
    r.MemInfo.dwTileCmpMark = tile;
    r.MemInfo.dwaaSecondaryColorBufBegin = 0;               /* one sample per chip */
    r.MemInfo.dwaaSecondaryDepthBufBegin = dbeg;
    r.MemInfo.dwaaSecondaryDepthBufEnd = dend;
    return r;
}

TEST(the_aa_lfb_control_that_froze_aa_and_its_fix_match_silicon) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_u32 v[4], d[4];
    int rc, c;
    /* cfg 6 = 2x AA: 2 SLI units x 2 samples, 1 sample per chip */
    vcr_sli_aa_req r6 = glide_req_124(1, 0, 0x038f6000u, 0x047f6080u, 0x04f76100u);
    /* cfg 7 = 4x AA: no SLI, 4 samples, 1 sample per chip */
    vcr_sli_aa_req r7 = glide_req_124(0, 1, 0x031ee000u, 0x040ee080u, 0x0486e100u);

    CHECK_EQ_U(vcr_sli_samples_per_chip(4, 1, 1, 0, 1), 1);
    CHECK_EQ_U(vcr_sli_samples_per_chip(4, 0, 1, 1, 1), 1);

    /* OLD - the dos_mode.c arm, cfg 6: base 0, i.e. AA LFB writes into offset 0.. */
    mapped(m, &io, 4); m->nw = 0;
    rc = vcr_sli_set_ex(&io, &r6, 0);
    CHECK(rc >= 0, "cfg 6 refused by the dos_mode.c arm");
    last_cfg_write_per_chip(m, VCR_CFG_AALFBCTRL, v);
    last_cfg_write_per_chip(m, VCR_CFG_AADEPTHBUFAPERTURE, d);
    for (c = 0; c < 4; c++) {
        CHECK_EQ_U(v[c], 0x4c000000u);                     /* read back 2026-09-30 09:39 */
        CHECK_EQ_U(v[c] & VCR_AALFB_READ_EN, 0);           /* THE FREEZE: no AA reads */
        CHECK_EQ_U(d[c], 0x4f7647f6u);
    }
    no_bus_faults(m);

    /* NEW - the vendor arm (the kernel's default since), cfg 6 */
    mapped(m, &io, 4); m->nw = 0;
    rc = vcr_sli_set_ex(&io, &r6, VCR_SLI_F_VENDOR_AA);
    CHECK(rc >= 0, "cfg 6 refused by the vendor arm");
    last_cfg_write_per_chip(m, VCR_CFG_AALFBCTRL, v);
    last_cfg_write_per_chip(m, VCR_CFG_AADEPTHBUFAPERTURE, d);
    for (c = 0; c < 4; c++) {
        CHECK_EQ_U(v[c], 0xdf8f6000u);                     /* read back 2026-09-30 12:59 */
        CHECK_EQ_U(v[c] & VCR_AALFB_SECONDARY_BASE_MASK, 0x038f6000u);   /* = tileMark */
        CHECK(v[c] & VCR_AALFB_READ_EN, "vendor arm: AA reads on");
        CHECK(v[c] & VCR_AALFB_RD_DIVIDE_BY_4, "vendor arm: /4");
        CHECK_EQ_U(d[c], 0x4f7647f6u);                     /* unchanged for SLI + 2x */
    }
    no_bus_faults(m);

    /* NEW - the vendor arm, cfg 7: chips 2/3 end with AA reads off (D:1439-1451)
     * and the depth aperture covers the whole tiled range */
    mapped(m, &io, 4); m->nw = 0;
    rc = vcr_sli_set_ex(&io, &r7, VCR_SLI_F_VENDOR_AA);
    CHECK(rc >= 0, "cfg 7 refused by the vendor arm");
    last_cfg_write_per_chip(m, VCR_CFG_AALFBCTRL, v);
    last_cfg_write_per_chip(m, VCR_CFG_AADEPTHBUFAPERTURE, d);
    CHECK_EQ_U(v[0], 0xdf1ee000u);                         /* read back 2026-09-30 13:22 */
    CHECK_EQ_U(v[1], 0xdf1ee000u);
    CHECK_EQ_U(v[2], 0xcf1ee000u);
    CHECK_EQ_U(v[3], 0xcf1ee000u);
    for (c = 0; c < 4; c++) {
        CHECK_EQ_U(v[c] & VCR_AALFB_SECONDARY_BASE_MASK, 0x031ee000u);   /* = tileMark */
        CHECK_EQ_U(d[c], 0x400031eeu);
    }
    no_bus_faults(m);

    /* cfg 8 = 8x: 2 samples per chip, a real secondary buffer. The vendor arm
     * before 2026-09-30 19:00 wrote 0xce3dc000 on all four chips and froze;
     * with READ_EN on chips 0/1 it ran clean (read back 19:13, boot #57) */
    {
        vcr_sli_aa_req r8 = glide_req_124(0, 2, 0x031ee000u, 0x040ee080u, 0x0486e100u);
        r8.MemInfo.dwaaSecondaryColorBufBegin = 0x023dc000u;
        CHECK_EQ_U(vcr_sli_samples_per_chip(4, 0, 1, 2, 1), 2);
        mapped(m, &io, 4); m->nw = 0;
        rc = vcr_sli_set_ex(&io, &r8, VCR_SLI_F_VENDOR_AA);
        CHECK(rc >= 0, "cfg 8 refused by the vendor arm");
        last_cfg_write_per_chip(m, VCR_CFG_AALFBCTRL, v);
        last_cfg_write_per_chip(m, VCR_CFG_AADEPTHBUFAPERTURE, d);
        CHECK_EQ_U(v[0], 0xde3dc000u);
        CHECK_EQ_U(v[1], 0xde3dc000u);
        CHECK_EQ_U(v[2], 0xce3dc000u);
        CHECK_EQ_U(v[3], 0xce3dc000u);
        for (c = 0; c < 4; c++) {
            CHECK_EQ_U(v[c] & VCR_AALFB_SECONDARY_BASE_MASK, 0x023dc000u);   /* the secondary */
            CHECK_EQ_U(d[c], 0x400031eeu);
        }
        CHECK(v[0] != 0xce3dc000u, "chips 0/1 lost READ_EN - the 8x freeze");
        no_bus_faults(m);
    }

    /* and the dos_mode.c arm would have written base 0 for cfg 7 too */
    mapped(m, &io, 4); m->nw = 0;
    rc = vcr_sli_set_ex(&io, &r7, 0);
    CHECK(rc >= 0, "cfg 7 refused by the dos_mode.c arm");
    last_cfg_write_per_chip(m, VCR_CFG_AALFBCTRL, v);
    for (c = 0; c < 4; c++)
        CHECK_EQ_U(v[c] & VCR_AALFB_SECONDARY_BASE_MASK, 0);
    no_bus_faults(m);
}

TEST(a_real_aa_base_cannot_spill_in_the_vendor_recipe) {
    static const struct { vcr_u32 col, old_chip0; } k[] = {
        /* 0x01a00000 << 4 = 0x1a000000: READ_EN (bit 28) set on chips 0/1 -
         * the scratch simulator's cfg-4-like case gave 0x9e000000 */
        { 0x01a00000u, 0x9e000000u },
        /* out of range: << 4 = 0x60000000, read format 3 (no such format) */
        { 0x06000000u, 0xe0000000u | 0x0c000000u },
    };
    mock *m = &M;
    vcr_sli_io io;
    unsigned i;
    vcr_u32 c, v;
    for (i = 0; i < 2; i++) {
        /* DEFAULT recipe, cfg 7's shape: dos_mode.c's spill, kept on purpose
         * as the control arm - the value 097b1f7 wrote */
        vcr_sli_aa_req r = req(4, 0, 1, 1, 1, 8, 16);
        r.MemInfo.dwaaSecondaryColorBufBegin = k[i].col;
        r.MemInfo.dwaaSecondaryDepthBufBegin = 0x01000000u;
        r.MemInfo.dwaaSecondaryDepthBufEnd = 0x01180000u;
        mapped(m, &io, 4);
        CHECK_EQ_I(vcr_sli_set(&io, &r), VCR_SLI_W_NOCLOCK);
        CHECK_EQ_U(CFG(m, 0, VCR_CFG_AALFBCTRL), k[i].old_chip0);
        /* what the old formula does to this very base */
        CHECK(((k[i].col << 4) & (VCR_AALFB_READ_EN | (3u << 29))) != 0, "demo case spills nothing");
        no_bus_faults(m);

        /* VENDOR recipe, a shape that keeps the request's own base (two
         * samples per chip: 2-way SLI + 4-sample): masked, no spill */
        r = req(4, 1, 1, 1, 1, 8, 16);
        r.MemInfo.dwaaSecondaryColorBufBegin = k[i].col;
        r.MemInfo.dwaaSecondaryDepthBufBegin = 0x01000000u;
        r.MemInfo.dwaaSecondaryDepthBufEnd = 0x01180000u;
        mapped(m, &io, 4);
        CHECK(vcr_sli_set_ex(&io, &r, VCR_SLI_F_VENDOR_AA) >= 0, "vendor 2-way SLI + 4-sample refused");
        for (c = 0; c < 4; c++) {
            v = CFG(m, c, VCR_CFG_AALFBCTRL);
            /* READ_EN is the recipe's own since 2026-09-30 (every AA shape of
             * a 4-chip board, as both 3dfx miniports; SLI here, so no chip
             * has it cleared) - the control bits are EXACTLY these, so a base
             * that spilled into them would still fail */
            CHECK_EQ_U(v & (3u << 29), VCR_AALFB_FMT_16BPP);     /* the format is 16 bpp */
            CHECK_EQ_U(v & VCR_AALFB_SECONDARY_BASE_MASK, k[i].col & VCR_AALFB_SECONDARY_BASE_MASK);
            CHECK_EQ_U(v & ~VCR_AALFB_SECONDARY_BASE_MASK,
                       VCR_AALFB_CPU_WRITE_EN | VCR_AALFB_DISPATCH_WRITE_EN | VCR_AALFB_READ_EN |
                       VCR_AALFB_RD_DIVIDE_BY_4);
            CHECK(v != k[i].old_chip0, "the vendor recipe spills");
        }
        no_bus_faults(m);
    }
}

/* Samples per chip, per shape. The 1-sample-per-chip set is what the vendor
 * recipe keys on; the vendor's own condition for "base = the primary buffers",
 * restated independently here, must pick the same three shapes. */
TEST(one_sample_per_chip_is_exactly_the_three_paired_shapes) {
    static const vcr_u32 ns[3] = { 1, 2, 4 };
    vcr_u32 ni, sli, aa, high, analog, ones = 0;
    for (ni = 0; ni < 3; ni++)
        for (sli = 0; sli < 2; sli++)
            for (aa = 0; aa < 2; aa++)
                for (high = 0; high < 3; high++)
                    for (analog = 0; analog < 2; analog++) {
                        vcr_u32 n = ns[ni], spc = vcr_sli_samples_per_chip(n, sli, aa, high, analog);
                        int vendor_primary = aa &&
                            ((n == 2 && !sli && !high) ||
                             (n == 4 && sli && !high && analog) ||
                             (n == 4 && !sli && high == 1 && analog));
                        if (!aa || !vcr_sli_combo_ok(n, sli, aa, high, analog)) {
                            CHECK_EQ_U(spc, 0);
                            continue;
                        }
                        CHECK(spc == 1 || spc == 2, "samples per chip out of range");
                        CHECK_EQ_I(spc == 1, vendor_primary);
                        ones += spc == 1;
                    }
    CHECK_EQ_U(ones, 4);                    /* {2,0,1,0,0}, {2,0,1,0,1}, cfg 3, cfg 7 */
    /* the named ones, and Glide's own layout (gsst.c: samplesPerChip) */
    CHECK_EQ_U(vcr_sli_samples_per_chip(4, 1, 1, 0, 1), 1);    /* cfg 3 */
    CHECK_EQ_U(vcr_sli_samples_per_chip(4, 0, 1, 1, 1), 1);    /* cfg 7 */
    CHECK_EQ_U(vcr_sli_samples_per_chip(4, 0, 1, 2, 1), 2);    /* cfg 8 */
    CHECK_EQ_U(vcr_sli_samples_per_chip(1, 0, 1, 0, 0), 2);    /* single-chip AA */
    CHECK_EQ_U(vcr_sli_samples_per_chip(4, 1, 1, 0, 0), 2);    /* 4-way digital + 2-sample */
    CHECK_EQ_U(vcr_sli_samples_per_chip(4, 1, 1, 1, 1), 2);    /* 2-way SLI + 4-sample */
    CHECK_EQ_U(vcr_sli_samples_per_chip(4, 0, 1, 0, 1), 0);    /* cfg 1 as sent: refused */
    CHECK_EQ_U(vcr_sli_samples_per_chip(4, 1, 0, 0, 1), 0);    /* no AA */
}

/* (b) the flag changes AA requests and nothing else: every pinned SLI-only
 * request and the disable write the identical bus sequence with it on */
TEST(the_vendor_recipe_leaves_sli_only_requests_and_the_disable_alone) {
    mock *m = &M;
    unsigned i, checked = 0;
    int rc;
    for (i = 0; i < sizeof k_seq / sizeof k_seq[0]; i++) {
        const seq_case *s = &k_seq[i];
        if (s->aa)
            continue;
        run_seq_ex(m, s, VCR_SLI_F_VENDOR_AA, &rc);
        if (rc != s->rc || m->nw != s->nw || wr_hash(m) != s->hash) {
            munit_fails++;
            fprintf(stderr, "    FAIL %s with the vendor recipe: rc %d nw %u hash 0x%08x\n",
                    s->name, rc, m->nw, wr_hash(m));
        }
        checked++;
    }
    CHECK_EQ_U(checked, 6);                 /* cfg 5 x3, the cfg 2/5 close, 2-chip SLI x2 */
}

TEST(the_vendor_recipe_refuses_memory_info_it_cannot_place) {
    static const struct { vcr_u32 tile, total; } bad[] = {
        { 0, 32u << 20 },                   /* no tileMark (vcrctl without one) */
        { 0x01b7e800u, 32u << 20 },         /* not on a 4 KB boundary */
        { 32u << 20, 32u << 20 },           /* tileMark at the end of memory */
        { 0x01b7e000u, 0 },                 /* no memory */
        { 0x04000000u, 0x05000000u },       /* past the base field's 64 MB */
    };
    mock *m = &M;
    vcr_sli_io io;
    unsigned i, t;
    vcr_u32 reason = 0, val = 0;
    /* cfg 3 (base = tileMark), cfg 7 (both), cfg 8 (the depth aperture) */
    static const vcr_u32 shapes[3][4] = { { 1, 1, 0, 1 }, { 0, 1, 1, 1 }, { 0, 1, 2, 1 } };
    for (t = 0; t < 3; t++)
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            vcr_sli_aa_req r = req(4, shapes[t][0], shapes[t][1], shapes[t][2], shapes[t][3], 8, 16);
            r.MemInfo.dwTileMark = bad[i].tile;
            r.MemInfo.dwTotalMemory = bad[i].total;
            mapped(m, &io, 4);
            m->nw = 0;
            CHECK_EQ_I(vcr_sli_set_ex(&io, &r, VCR_SLI_F_VENDOR_AA), VCR_SLI_EINVAL);
            CHECK_EQ_U(m->nw, 0);
            CHECK(last_refusal(m, &reason, &val), "refusal not logged");
            CHECK_EQ_U(reason, VCR_SLI_R_MEMINFO);
            CHECK_EQ_U(val, bad[i].tile >> 12);
            /* the default recipe never reads them: the same request runs */
            mapped(m, &io, 4);
            CHECK(vcr_sli_set(&io, &r) >= 0, "the default recipe refused over memory info");
        }
    /* shapes that do not use them run with the recipe on and no tileMark */
    {
        static const vcr_u32 fine[3][5] = {
            { 1, 0, 1, 0, 0 },              /* single-chip AA: 2 samples per chip */
            { 4, 1, 1, 1, 1 },              /* 2-way SLI + 4-sample: 2 per chip, SLI */
            { 4, 1, 1, 0, 0 },              /* 4-way digital SLI + 2-sample */
        };
        for (t = 0; t < 3; t++) {
            vcr_sli_aa_req r = req(fine[t][0], fine[t][1], fine[t][2], fine[t][3], fine[t][4], 8, 16);
            mapped(m, &io, 4);
            CHECK(vcr_sli_set_ex(&io, &r, VCR_SLI_F_VENDOR_AA) >= 0, "refused a shape that needs no tileMark");
        }
    }
}

/* (d) The expected config space of cfg 3, cfg 7 and cfg 1 on the V5 6000,
 * for BOTH recipes - the table a supervised config read-back (`vcrctl pci`,
 * Diag\SliAAState) is compared with.
 *
 * The dos_mode columns are the scratch simulator's (aa-hw, 2026-09-27: the
 * old vcrmp_sli.c against a mock seeded from the AmigaMerlin cfg 0 golden,
 * which reproduced golden sli_amigamerlin-3.1-r11_cfg5 exactly); this mock
 * reproduces them - except the fab-ID byte of 0x40, which it keeps read-only
 * like the chip (0x..01), and pciInit0, whose seed here is not the golden's.
 * The vendor columns differ from them only where vcr_sli.h says they do.
 * Request: nlines 8, 16 bpp, tileMark 0x01b7e000, 32 MB, col 0, depth
 * 0x01000000-0x01180000 (representative values, not a measured request).
 * cfg 8 (8-sample, two samples per chip, so it has a real secondary buffer:
 * col 0x00b00000) closes the table: it is the other half of the vendor
 * recipe's "4 chips, no SLI, 4- or 8-sample" depth-aperture rule, and the one
 * shape whose two rows differ in the BASE: dos_mode.c 0x8f000000 (D:871's
 * << 4, what the simulator printed from the old code), vendor 0x8cb00000 (the
 * byte address, header difference 13) - 0x9cb00000 on chips 0/1 since
 * 2026-09-30 (AA reads on, as both 3dfx miniports). */
#define T_TILE      0x01b7e000u
#define T_TOTAL     (32u << 20)
#define T_WHOLE     ((T_TILE >> 12) | ((T_TOTAL >> 12) << 16))     /* 0x20001b7e */
#define T_DEPTH     0x11801000u                                    /* 0x01000000-0x01180000 */
#define T_VAA       0x9db7e000u     /* tileMark, CPU+dispatch write, READ_EN, 16 bpp, /4 */

typedef struct {
    vcr_u32 cfg[VCR_SLI_STATE_NCFG];    /* 0x40 0x48 0x80 0x84 0x88 0x8c 0x90 0x94 0xac */
    vcr_u32 slictrl;
    unsigned slictrl_writes;
} chip_expect;

typedef struct {
    const char *name;
    vcr_u32 n, sli, aa, high, analog, flags;
    int rc;
    chip_expect chip[4];
    vcr_u32 col;                        /* dwaaSecondaryColorBufBegin (0 unless given) */
} aa_table;

#define IE0  0x06000b01u
#define IES  0x4ba07b01u
#define DEC0 0x00000045u
#define DECS 0x0c011445u

static const aa_table k_aa_tables[] = {
    { "cfg 3 {4,1,1,0,1}, dos_mode.c recipe", 4, 1, 1, 0, 1, 0, VCR_SLI_W_NOCLOCK, {
      { { IE0, DEC0, 0x00001811u, 0x00080008u, 0,       0x1d070008u, T_DEPTH, 0x0c000000u, 0x0800u }, 0x05070008u, 1 },
      { { IES, DECS, 0x02000803u, 0xf8000008u, 0,       0x1d070008u, T_DEPTH, 0x0c000000u, 0x082fu }, 0x05070008u, 1 },
      { { IES, DECS, 0x00001843u, 0x08080808u, 0xff00u, 0x1d070808u, T_DEPTH, 0x0c000000u, 0x0827u }, 0x05070808u, 1 },
      { { IES, DECS, 0x02000803u, 0xf8000808u, 0,       0x1d070808u, T_DEPTH, 0x0c000000u, 0x182fu }, 0x05070808u, 1 } }, 0 },
    { "cfg 3 {4,1,1,0,1}, vendor recipe", 4, 1, 1, 0, 1, VCR_SLI_F_VENDOR_AA, VCR_SLI_W_NOCLOCK, {
      { { IE0, DEC0, 0x00001811u, 0x00080008u, 0,       0x1d070008u, T_DEPTH, T_VAA, 0x0800u }, 0x05070008u, 1 },
      { { IES, DECS, 0x02000803u, 0xf8000008u, 0,       0x1d070008u, T_DEPTH, T_VAA, 0x082fu }, 0x05070008u, 1 },
      { { IES, DECS, 0x00001843u, 0x08080808u, 0xff00u, 0x1d070808u, T_DEPTH, T_VAA, 0x0827u }, 0x05070808u, 1 },
      { { IES, DECS, 0x02000803u, 0xf8000808u, 0,       0x1d070808u, T_DEPTH, T_VAA, 0x182fu }, 0x05070808u, 1 } }, 0 },
    { "cfg 7 {4,0,1,1,1}, dos_mode.c recipe", 4, 0, 1, 1, 1, 0, VCR_SLI_W_NOCLOCK, {
      { { IE0, DEC0, 0x00002811u, 0,           0,       0, T_DEPTH, 0x8c000000u, 0x0800u }, 0, 0 },
      { { IES, DECS, 0x02000803u, 0xff000000u, 0,       0, T_DEPTH, 0x8c000000u, 0x082fu }, 0, 0 },
      { { IES, DECS, 0x02002843u, 0,           0xff00u, 0, T_DEPTH, 0x8c000000u, 0x0827u }, 0, 0 },
      { { IES, DECS, 0x02000803u, 0xff000000u, 0,       0, T_DEPTH, 0x8c000000u, 0x082fu }, 0, 0 } }, 0 },
    /* chips 2/3: READ_EN set, then cleared by D:1439-1451 (AA reads off) */
    { "cfg 7 {4,0,1,1,1}, vendor recipe", 4, 0, 1, 1, 1, VCR_SLI_F_VENDOR_AA, VCR_SLI_W_NOCLOCK, {
      { { IE0, DEC0, 0x00002811u, 0,           0,       0, T_WHOLE, T_VAA,                         0x0800u }, 0, 1 },
      { { IES, DECS, 0x02000803u, 0xff000000u, 0,       0, T_WHOLE, T_VAA,                         0x082fu }, 0, 1 },
      { { IES, DECS, 0x02002843u, 0,           0xff00u, 0, T_WHOLE, T_VAA & ~VCR_AALFB_READ_EN,    0x0827u }, 0, 1 },
      { { IES, DECS, 0x02000803u, 0xff000000u, 0,       0, T_WHOLE, T_VAA & ~VCR_AALFB_READ_EN,    0x082fu }, 0, 1 } }, 0 },
    /* cfg 1 as LABELLED: one chip, 2 samples on it - 2 per chip, so the
     * vendor recipe adds only sliCtrl = 0. (As Glide SENDS it - {4,0,1,0,1} -
     * both recipes refuse it: cfg1_is_refused_by_both_recipes.) Chips 1-3 of
     * the board are not part of the request and are not touched. */
    { "cfg 1 as labelled {1,0,1,0,0}, dos_mode.c recipe", 1, 0, 1, 0, 0, 0, VCR_SLI_W_NOCLOCK, {
      { { 0x00000301u, DEC0, 0x00001009u, 0, 0xff00u, 0, T_DEPTH, 0x0c000000u, 0x0800u }, 0, 0 } }, 0 },
    { "cfg 1 as labelled {1,0,1,0,0}, vendor recipe", 1, 0, 1, 0, 0, VCR_SLI_F_VENDOR_AA, VCR_SLI_W_NOCLOCK, {
      { { 0x00000301u, DEC0, 0x00001009u, 0, 0xff00u, 0, T_DEPTH, 0x0c000000u, 0x0800u }, 0, 1 } }, 0 },
    /* cfg 8: the vendor recipe writes the base as a byte address, the depth
     * aperture as the whole tiled range and sliCtrl = 0 - two samples per chip
     * keep the request's own base, no AA READ_EN; chips 2/3 still get
     * D:1439's (no-op) READ_EN clear */
    { "cfg 8 {4,0,1,2,1}, dos_mode.c recipe", 4, 0, 1, 2, 1, 0, VCR_SLI_W_NOCLOCK, {
      { { IE0, DEC0, 0x00003819u, 0,           0,       0, T_DEPTH, CFG8_AALFB_OLD, 0x0800u }, 0, 0 },
      { { IES, DECS, 0x0200080bu, 0xff000000u, 0,       0, T_DEPTH, CFG8_AALFB_OLD, 0x082fu }, 0, 0 },
      { { IES, DECS, 0x0200384bu, 0,           0xff00u, 0, T_DEPTH, CFG8_AALFB_OLD, 0x0827u }, 0, 0 },
      { { IES, DECS, 0x0200080bu, 0xff000000u, 0,       0, T_DEPTH, CFG8_AALFB_OLD, 0x082fu }, 0, 0 } },
      0x00b00000u },
    { "cfg 8 {4,0,1,2,1}, vendor recipe", 4, 0, 1, 2, 1, VCR_SLI_F_VENDOR_AA, VCR_SLI_W_NOCLOCK, {
      { { IE0, DEC0, 0x00003819u, 0,           0,       0, T_WHOLE, CFG8_AALFB_NEW_RD, 0x0800u }, 0, 1 },
      { { IES, DECS, 0x0200080bu, 0xff000000u, 0,       0, T_WHOLE, CFG8_AALFB_NEW_RD, 0x082fu }, 0, 1 },
      { { IES, DECS, 0x0200384bu, 0,           0xff00u, 0, T_WHOLE, CFG8_AALFB_NEW, 0x0827u }, 0, 1 },
      { { IES, DECS, 0x0200080bu, 0xff000000u, 0,       0, T_WHOLE, CFG8_AALFB_NEW, 0x082fu }, 0, 1 } },
      0x00b00000u },
    /* the cfg 3 ghost arms (2026-09-28, vcr_sli.h): the AA-FIFO gate changes
     * only 0x88 on every chip (each chip's fetch band: 0x08/0 for chips 0/1,
     * 0x08/0x08 for chips 2/3 at 8-line bands) and chip 2's sum mux (0x1843
     * -> 0x1813); the feeder lead only a feeder's 0xAC chars 5 -> 4 */
    { "cfg 3 {4,1,1,0,1}, AA-FIFO gate arm", 4, 1, 1, 0, 1, VCR_SLI_F_AAFIFO_GATE, VCR_SLI_W_NOCLOCK, {
      { { IE0, DEC0, 0x00001811u, 0x00080008u, 0x0008u, 0x1d070008u, T_DEPTH, 0x0c000000u, 0x0800u }, 0x05070008u, 1 },
      { { IES, DECS, 0x02000803u, 0xf8000008u, 0x0008u, 0x1d070008u, T_DEPTH, 0x0c000000u, 0x082fu }, 0x05070008u, 1 },
      { { IES, DECS, 0x00001813u, 0x08080808u, 0x0808u, 0x1d070808u, T_DEPTH, 0x0c000000u, 0x0827u }, 0x05070808u, 1 },
      { { IES, DECS, 0x02000803u, 0xf8000808u, 0x0808u, 0x1d070808u, T_DEPTH, 0x0c000000u, 0x182fu }, 0x05070808u, 1 } }, 0 },
    { "cfg 3 {4,1,1,0,1}, feeder-lead arm, chip 1 only", 4, 1, 1, 0, 1, VCR_SLI_F_FEEDER_LEAD_C1, VCR_SLI_W_NOCLOCK, {
      { { IE0, DEC0, 0x00001811u, 0x00080008u, 0,       0x1d070008u, T_DEPTH, 0x0c000000u, 0x0800u }, 0x05070008u, 1 },
      { { IES, DECS, 0x02000803u, 0xf8000008u, 0,       0x1d070008u, T_DEPTH, 0x0c000000u, 0x0827u }, 0x05070008u, 1 },
      { { IES, DECS, 0x00001843u, 0x08080808u, 0xff00u, 0x1d070808u, T_DEPTH, 0x0c000000u, 0x0827u }, 0x05070808u, 1 },
      { { IES, DECS, 0x02000803u, 0xf8000808u, 0,       0x1d070808u, T_DEPTH, 0x0c000000u, 0x182fu }, 0x05070808u, 1 } }, 0 },
    { "cfg 3 {4,1,1,0,1}, feeder-lead arm, chips 1 and 3", 4, 1, 1, 0, 1,
      VCR_SLI_F_FEEDER_LEAD_C1 | VCR_SLI_F_FEEDER_LEAD_C3, VCR_SLI_W_NOCLOCK, {
      { { IE0, DEC0, 0x00001811u, 0x00080008u, 0,       0x1d070008u, T_DEPTH, 0x0c000000u, 0x0800u }, 0x05070008u, 1 },
      { { IES, DECS, 0x02000803u, 0xf8000008u, 0,       0x1d070008u, T_DEPTH, 0x0c000000u, 0x0827u }, 0x05070008u, 1 },
      { { IES, DECS, 0x00001843u, 0x08080808u, 0xff00u, 0x1d070808u, T_DEPTH, 0x0c000000u, 0x0827u }, 0x05070808u, 1 },
      { { IES, DECS, 0x02000803u, 0xf8000808u, 0,       0x1d070808u, T_DEPTH, 0x0c000000u, 0x1827u }, 0x05070808u, 1 } }, 0 },
};

static const vcr_u32 k_state_offs[VCR_SLI_STATE_NCFG] = {
    0x40, 0x48, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0xac
};

static vcr_sli_aa_req table_req(const aa_table *t)
{
    vcr_sli_aa_req r = req(t->n, t->sli, t->aa, t->high, t->analog, 8, 16);
    r.MemInfo.dwTileMark = T_TILE;
    r.MemInfo.dwTileCmpMark = T_TILE;
    r.MemInfo.dwTotalMemory = T_TOTAL;
    r.MemInfo.dwaaSecondaryDepthBufBegin = 0x01000000u;
    r.MemInfo.dwaaSecondaryDepthBufEnd = 0x01180000u;
    r.MemInfo.dwaaSecondaryColorBufBegin = t->col;
    return r;
}

/* what the kernel's k_log captures: pciInit0 as each chip's PCIINIT0 step wrote it */
static vcr_u32 logged_pciinit0(const mock *m, vcr_u32 pci0[4])
{
    unsigned i;
    vcr_u32 mask = 0;
    for (i = 0; i < m->nl; i++)
        if (m->l[i].step == VCR_SLI_S_PCIINIT0 && m->l[i].chip < 4) {
            pci0[m->l[i].chip] = m->l[i].val;
            mask |= 1u << m->l[i].chip;
        }
    return mask;
}

TEST(aa_config_space_matches_the_expected_tables_for_both_recipes) {
    mock *m = &M;
    unsigned t, c, i;
    for (t = 0; t < sizeof k_aa_tables / sizeof k_aa_tables[0]; t++) {
        const aa_table *e = &k_aa_tables[t];
        vcr_sli_io io;
        vcr_sli_aa_req r = table_req(e);
        int rc;
        mapped(m, &io, 4);
        rc = vcr_sli_set_ex(&io, &r, e->flags);
        if (rc != e->rc) {
            munit_fails++;
            fprintf(stderr, "    FAIL %s: rc %d, expected %d\n", e->name, rc, e->rc);
        }
        for (c = 0; c < e->n; c++) {
            for (i = 0; i < VCR_SLI_STATE_NCFG; i++)
                if (CFG(m, c, k_state_offs[i]) != e->chip[c].cfg[i]) {
                    munit_fails++;
                    fprintf(stderr, "    FAIL %s: chip %u cfg %02x = %08x, table %08x\n", e->name,
                            c, k_state_offs[i], CFG(m, c, k_state_offs[i]), e->chip[c].cfg[i]);
                }
            CHECK_EQ_U(m->slictrl[c], e->chip[c].slictrl);
            CHECK_EQ_U(m->slictrl_direct[c], e->chip[c].slictrl_writes);
            /* D:730-768 on every chip of the request */
            CHECK_EQ_U(IOR(m, c, VCR_R_PCIINIT0), 0x00000303u);
        }
        for (; c < 4; c++)                  /* chips outside the request: untouched */
            CHECK_EQ_U(CFG(m, c, VCR_CFG_VIDEOCTRL0), 0);
        CHECK(!has_step(m, VCR_SLI_S_NOMUX), "NOMUX reached");
        no_bus_faults(m);
    }
}

/* cfg 1 AS GLIDE SENDS IT ({4,0,1,0,1}) under the kernel BEFORE the safety net
 * (097b1f7): the config space .124 froze in at boot #18 (HWC_SLIAA a=4
 * b=0x102, SET_DONE, then the hang inside grSstWinOpen -
 * evidence/glidelab/postmortem_20260927). Kept here because the aa-hw scratch
 * simulator that produced it (the old vcrmp_sli.c against a mock seeded from
 * the AmigaMerlin cfg 0 golden, fab-ID byte 00) is deleted: rc NOCLOCK|NOMUX,
 * full snoop + swap sync on every slave, VIDPLL_SEL with no video mux, the AA
 * apertures written, pciInit0 0x01841328 on every chip. Both recipes now
 * refuse the request with none of it written. Order as k_state_offs. */
static const vcr_u32 k_cfg1_sent_old[4][VCR_SLI_STATE_NCFG] = {
    { 0x06000b00u, 0x00000045u, 0x00000800u, 0, 0, 0, 0x11801000u, 0x0c000000u, 0x0800u },
    { 0x4ba07b00u, 0x0c011445u, 0x03000800u, 0, 0, 0, 0x11801000u, 0x0c000000u, 0x0827u },
    { 0x4ba07b00u, 0x0c011445u, 0x03000800u, 0, 0, 0, 0x11801000u, 0x0c000000u, 0x0827u },
    { 0x4ba07b00u, 0x0c011445u, 0x03000800u, 0, 0, 0, 0x11801000u, 0x0c000000u, 0x0827u },
};
#define CFG1_OLD_PCIINIT0   0x01841328u     /* every chip, as the simulator wrote it (golden seed) */

TEST(cfg1_is_refused_by_both_recipes) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_u32 flags, c, i;
    for (flags = 0; flags <= VCR_SLI_F_VENDOR_AA; flags++) {
        aa_table t = { "cfg 1 as sent", 4, 0, 1, 0, 1, 0, 0, { { { 0 }, 0, 0 } }, 0 };
        vcr_sli_aa_req r = table_req(&t);
        vcr_sli_aa_state st;
        vcr_u32 reason = 0, val = 0, before[4][VCR_SLI_STATE_NCFG], pci0_before[4];
        mapped(m, &io, 4);
        for (c = 0; c < 4; c++) {
            for (i = 0; i < VCR_SLI_STATE_NCFG; i++)
                before[c][i] = CFG(m, c, k_state_offs[i]);
            pci0_before[c] = IOR(m, c, VCR_R_PCIINIT0);
        }
        m->nw = 0;
        m->nl = 0;
        CHECK_EQ_I(vcr_sli_set_ex(&io, &r, flags), VCR_SLI_EINVAL);
        CHECK_EQ_U(m->nw, 0);
        CHECK(last_refusal(m, &reason, &val) && reason == VCR_SLI_R_COMBO, "not a COMBO refusal");
        /* the fixed state: every register as it was before the request; the
         * old state: what the pre-safety-net kernel had left in them */
        for (c = 0; c < 4; c++) {
            for (i = 0; i < VCR_SLI_STATE_NCFG; i++)
                CHECK_EQ_U(CFG(m, c, k_state_offs[i]), before[c][i]);
            CHECK_EQ_U(IOR(m, c, VCR_R_PCIINIT0), pci0_before[c]);
            CHECK(CFG(m, c, VCR_CFG_VIDEOCTRL0) != k_cfg1_sent_old[c][2], "VIDPLL_SEL written again");
            CHECK(CFG(m, c, VCR_CFG_AADEPTHBUFAPERTURE) != k_cfg1_sent_old[c][6], "AA depth aperture written");
            CHECK(CFG(m, c, VCR_CFG_AALFBCTRL) != k_cfg1_sent_old[c][7], "cfgAALfbCtrl written again");
            /* (CFG1_OLD_PCIINIT0 is recorded, not compared: this mock's pciInit0
             * seed is not the golden's; the equality with `before` above is
             * what catches a pciInit0 write) */
            if (c)                          /* snoop + swap sync, fab-ID byte aside */
                CHECK((CFG(m, c, VCR_CFG_INITENABLE) & ~0xffu) != (k_cfg1_sent_old[c][0] & ~0xffu),
                      "a slave snoops the master again");
        }
        /* and a refused request is never read back */
        m->cfg_reads = 0;
        CHECK_EQ_I(vcr_sli_aa_readback(&io, &r, VCR_SLI_EINVAL, flags, NULL, 0, &st), 0);
        CHECK_EQ_U(m->cfg_reads, 0);
        CHECK_EQ_U(st.magic, 0);
    }
}

/* (c) Diag\SliAAState: the read-back uses config cycles and nothing else,
 * only after an AA enable, and records exactly the table's registers */
TEST(the_aa_state_is_read_back_by_config_cycles_only) {
    mock *m = &M;
    unsigned t, c, i;
    for (t = 0; t < sizeof k_aa_tables / sizeof k_aa_tables[0]; t++) {
        const aa_table *e = &k_aa_tables[t];
        vcr_sli_io io;
        vcr_sli_aa_req r = table_req(e);
        vcr_sli_aa_state st;
        vcr_u32 pci0[4] = { 0 }, mask;
        unsigned nw;
        unsigned long bar;
        int rc, n;
        mapped(m, &io, 4);
        m->nl = 0;
        rc = vcr_sli_set_ex(&io, &r, e->flags);
        mask = logged_pciinit0(m, pci0);
        CHECK_EQ_U(mask, (1u << e->n) - 1);
        nw = m->nw;
        bar = m->bar_ops;
        m->cfg_reads = 0;
        m->nl = 0;
        CHECK(vcr_sli_aa_state_wanted(&r, rc), "an AA enable not read back");
        n = vcr_sli_aa_readback(&io, &r, rc, e->flags, pci0, mask, &st);
        CHECK_EQ_I(n, (int)e->n);
        CHECK_EQ_U(m->nw, nw);                              /* no write */
        CHECK_EQ_U(m->bar_ops, bar);                        /* no BAR, no VGA */
        CHECK_EQ_U(m->cfg_reads, e->n * VCR_SLI_STATE_NCFG); /* config reads, that's all */
        /* each chip announced before its reads, then the end */
        CHECK_EQ_U(m->nl, e->n + 1);
        for (c = 0; c < e->n; c++)
            CHECK(m->l[c].step == VCR_SLI_S_AA_STATE && m->l[c].chip == c && m->l[c].reg == 0,
                  "chip not announced before its reads");
        CHECK(m->l[e->n].step == VCR_SLI_S_AA_STATE && m->l[e->n].reg == 1, "no end step");
        CHECK_EQ_U(st.magic, VCR_SLI_STATE_MAGIC);
        CHECK_EQ_U(st.size, VCR_SLI_STATE_BYTES);
        CHECK_EQ_U(st.tuple, vcr_sli_req_tuple(&r));
        CHECK_EQ_U(st.flags, e->flags | (mask << 8));
        CHECK_EQ_I(st.result, e->rc);
        CHECK_EQ_U(st.nchips, e->n);
        CHECK_EQ_U(st.tile, T_TILE);
        CHECK_EQ_U(st.total, T_TOTAL);
        CHECK_EQ_U(st.nlines, 8);
        CHECK_EQ_U(st.bpp, 16);
        CHECK_EQ_U(st.col, e->col);
        CHECK_EQ_U(st.dbeg, 0x01000000u);
        CHECK_EQ_U(st.dend, 0x01180000u);
        CHECK_EQ_U(st.boot, 0);                             /* the kernel's to fill */
        for (c = 0; c < 4; c++) {
            for (i = 0; i < VCR_SLI_STATE_NCFG; i++)
                CHECK_EQ_U(st.chip[c].cfg[i], c < e->n ? e->chip[c].cfg[i] : 0);
            CHECK_EQ_U(st.chip[c].pciinit0, c < e->n ? 0x00000303u : 0);
        }
        /* a chip whose pciInit0 write the kernel did not see is "not recorded",
         * never a stale value: only the mask's chips carry one */
        if (e->n == 4) {
            vcr_u32 stale[4] = { 0x11u, 0x22u, 0x33u, 0x44u };
            CHECK_EQ_I(vcr_sli_aa_readback(&io, &r, rc, e->flags, stale, 0x5, &st), 4);
            CHECK_EQ_U(st.flags, e->flags | VCR_SLI_ST_PCI0(0) | VCR_SLI_ST_PCI0(2));
            CHECK_EQ_U(st.chip[0].pciinit0, 0x11u);
            CHECK_EQ_U(st.chip[1].pciinit0, 0);
            CHECK_EQ_U(st.chip[2].pciinit0, 0x33u);
            CHECK_EQ_U(st.chip[3].pciinit0, 0);
        }
    }
}

TEST(no_state_record_without_an_aa_enable) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_sli_aa_state st;
    vcr_sli_aa_req r = req(4, 1, 0, 0, 1, 8, 16);           /* cfg 5: SLI only */
    vcr_u32 pci0[4] = { 1, 2, 3, 4 };
    int rc;
    mapped(m, &io, 4);
    rc = vcr_sli_set(&io, &r);
    CHECK(!vcr_sli_aa_state_wanted(&r, rc), "an SLI-only request wants a state record");
    m->cfg_reads = 0;
    memset(&st, 0xa5, sizeof st);
    CHECK_EQ_I(vcr_sli_aa_readback(&io, &r, rc, 0, pci0, 0xf, &st), 0);
    CHECK_EQ_U(m->cfg_reads, 0);
    CHECK_EQ_U(st.magic, 0);                                /* zeroed, not left as it was */
    CHECK_EQ_U(st.chip[3].pciinit0, 0);
    /* an AA request that failed, and no request at all */
    r = req(4, 0, 1, 1, 1, 8, 16);
    CHECK(!vcr_sli_aa_state_wanted(&r, VCR_SLI_EDENIED), "a refused request wants a record");
    CHECK(vcr_sli_aa_state_wanted(&r, VCR_SLI_W_NOCLOCK), "done-with-warnings is still done");
    CHECK(!vcr_sli_aa_state_wanted(NULL, 0), "no request wants a record");
    CHECK_EQ_I(vcr_sli_aa_readback(NULL, &r, 0, 0, pci0, 0xf, &st), 0);
    CHECK_EQ_I(vcr_sli_aa_readback(&io, &r, 0, 0, pci0, 0xf, NULL), 0);
    CHECK_EQ_U(sizeof(vcr_sli_aa_state), VCR_SLI_STATE_BYTES);
    CHECK_EQ_U(offsetof(vcr_sli_aa_state, chip), 64);
}

TEST(the_recipe_is_visible_in_the_persisted_phases) {
    mock *m = &M;
    vcr_sli_io io;
    vcr_sli_aa_req r = table_req(&k_aa_tables[3]);          /* cfg 7, vendor */
    unsigned i;
    int begin = 0, aaonly = 0;
    mapped(m, &io, 4);
    m->nl = 0;
    CHECK(vcr_sli_set_ex(&io, &r, VCR_SLI_F_VENDOR_AA) >= 0, "cfg 7 vendor refused");
    for (i = 0; i < m->nl; i++) {
        if (m->l[i].step == VCR_SLI_S_SET_BEGIN) {
            begin = 1;
            CHECK(m->l[i].reg & 0x100, "SET_BEGIN does not say vendor recipe");
            /* persisted as chip << 24 | reg: the bit survives */
            CHECK(vcr_sli_phase_b(VCR_SLI_S_SET_BEGIN, m->l[i].chip, m->l[i].reg, m->l[i].val) & 0x100,
                  "the recipe bit is not in the persisted phase");
        }
        aaonly += m->l[i].step == VCR_SLI_S_AAONLY_SLICTRL;
    }
    CHECK(begin, "no SET_BEGIN");
    CHECK_EQ_I(aaonly, 4);
    CHECK(vcr_sli_step_persists(VCR_SLI_S_AA_STATE, 0), "AA_STATE not persisted");
    CHECK(vcr_sli_step_persists(VCR_SLI_S_AAONLY_SLICTRL, 0), "AAONLY_SLICTRL not persisted");
    /* the default recipe logs SET_BEGIN exactly as before */
    mapped(m, &io, 4);
    m->nl = 0;
    CHECK(vcr_sli_set(&io, &r) >= 0, "cfg 7 refused");
    for (i = 0; i < m->nl; i++) {
        CHECK(m->l[i].step != VCR_SLI_S_AAONLY_SLICTRL, "the default recipe wrote sliCtrl");
        if (m->l[i].step == VCR_SLI_S_SET_BEGIN)
            CHECK_EQ_U(m->l[i].reg, 0x2u | 0x4u | 0x10u);    /* aa, analog, sampleHigh 1 */
    }
}

/* (f) the cfg 3 ghost arms change the cfg 3 shape and nothing else: every
 * other pinned request, and the disable, write the identical bus sequence
 * with all of them on - and cfg 3 with them off is the pinned sequence */
TEST(the_cfg3_arms_leave_every_other_shape_and_the_disable_alone) {
    mock *m = &M;
    unsigned i, checked = 0;
    int rc;
    for (i = 0; i < sizeof k_seq / sizeof k_seq[0]; i++) {
        const seq_case *s = &k_seq[i];
        int cfg3 = s->n == 4 && s->sli && s->aa && !s->high && s->analog && !s->then_disable;
        run_seq_ex(m, s, VCR_SLI_F_CFG3_ARMS, &rc);
        if (cfg3) {
            /* the same number of writes (an arm replaces values, adds none) */
            CHECK_EQ_U(m->nw, s->nw);
            CHECK(wr_hash(m) != s->hash, "the cfg 3 arms changed nothing");
            continue;
        }
        if (rc != s->rc || m->nw != s->nw || wr_hash(m) != s->hash) {
            munit_fails++;
            fprintf(stderr, "    FAIL %s with the cfg 3 arms: rc %d nw %u hash 0x%08x\n",
                    s->name, rc, m->nw, wr_hash(m));
        }
        checked++;
    }
    CHECK_EQ_U(checked, 8);     /* cfg 5 x3, the close, 2-chip SLI x2, cfg 7, cfg 8 */
}

/* the arm is named in SET_BEGIN (bit 9 = gate, bits 10-11 = feeder lead), in
 * the persisted phase too; without an arm SET_BEGIN is what it was */
TEST(the_cfg3_arms_are_visible_in_the_persisted_phases) {
    static const vcr_u32 arms[3] = { VCR_SLI_F_AAFIFO_GATE, VCR_SLI_F_FEEDER_LEAD_C1,
                                     VCR_SLI_F_FEEDER_LEAD_C1 | VCR_SLI_F_FEEDER_LEAD_C3 };
    static const vcr_u32 bits[3] = { 0x200u, 0x400u, 0xc00u };
    mock *m = &M;
    vcr_sli_io io;
    vcr_sli_aa_req r = table_req(&k_aa_tables[0]);          /* cfg 3, dos_mode.c */
    unsigned a, i;
    for (a = 0; a < 3; a++) {
        int begin = 0;
        mapped(m, &io, 4);
        m->nl = 0;
        CHECK(vcr_sli_set_ex(&io, &r, arms[a]) >= 0, "cfg 3 with an arm refused");
        for (i = 0; i < m->nl; i++)
            if (m->l[i].step == VCR_SLI_S_SET_BEGIN) {
                begin = 1;
                CHECK_EQ_U(m->l[i].reg & 0xe00u, bits[a]);
                CHECK_EQ_U(vcr_sli_phase_b(VCR_SLI_S_SET_BEGIN, m->l[i].chip, m->l[i].reg,
                                           m->l[i].val) & 0xe00u, bits[a]);
            }
        CHECK(begin, "no SET_BEGIN");
    }
    mapped(m, &io, 4);
    m->nl = 0;
    CHECK(vcr_sli_set(&io, &r) >= 0, "cfg 3 refused");
    for (i = 0; i < m->nl; i++)
        if (m->l[i].step == VCR_SLI_S_SET_BEGIN)
            CHECK_EQ_U(m->l[i].reg, 0x1u | 0x2u | 0x4u);    /* sli, aa, analog */
}

/* no arm, alone or together, on any shape, can put a vga_vsync_offset of
 * pixels 7 / chars 3 (31 px, 0x1f) in any chip: it hard-froze the V5 6000 */
TEST(no_arm_can_reach_the_vsync_offset_that_froze_the_board) {
    mock *m = &M;
    unsigned i, c, f;
    int rc;
    for (f = 0; f <= VCR_SLI_F_CFG3_ARMS; f += 2)
        for (i = 0; i < sizeof k_seq / sizeof k_seq[0]; i++) {
            const seq_case *s = &k_seq[i];
            if (s->then_disable)
                continue;
            run_seq_ex(m, s, f | VCR_SLI_F_VENDOR_AA, &rc);
            for (c = 1; c < s->n; c++) {
                vcr_u32 chars = (CFG(m, c, VCR_CFG_SLIAAMISC) & VCR_SLIAA_VSYNC_OFFSET) >>
                                VCR_SLIAA_VSYNC_CHARS_SHIFT;
                if (rc >= 0 && (s->aa || s->sli))
                    CHECK(chars == 4 || chars == 5, "a vsync offset other than 39/47 px");
            }
        }
}

/* (e) `vcrctl sliaa`: the parser, the at-the-box gate, Glide's request */
static const char *sliaa(vcr_sliaa_cmd *c, int argc, ...)
{
    static char buf[16][32];
    char *argv[16];
    va_list ap;
    int i;
    va_start(ap, argc);
    for (i = 0; i < argc && i < 16; i++) {
        strncpy(buf[i], va_arg(ap, const char *), sizeof buf[i] - 1);
        buf[i][sizeof buf[i] - 1] = 0;
        argv[i] = buf[i];
    }
    va_end(ap);
    return vcr_sliaa_parse(argc, argv, c);
}

/* the gate ea90707 shipped: AA without the flag, nothing else */
static const char *sliaa_gate_old(const vcr_sliaa_cmd *c)
{
    return (!c->off && c->aa && !c->at_box) ? "refused" : 0;
}

TEST(vcrctl_sliaa_refuses_every_enable_without_a_person_at_the_box) {
    vcr_sliaa_cmd c;
    vcr_sli_aa_req r;
    /* cfg 3 as a kernel probe: parses, and is REFUSED without the flag */
    CHECK(!sliaa(&c, 5, "4", "1", "1", "0", "1"), "cfg 3 did not parse");
    CHECK(vcr_sliaa_gate(&c) != 0, "an AA request passed the gate with nobody at the box");
    CHECK(!sliaa(&c, 6, "4", "0", "1", "1", "1", VCR_SLIAA_AT_BOX_FLAG), "cfg 7 + flag");
    CHECK(vcr_sliaa_gate(&c) == 0, "the flag did not open the gate");
    CHECK(!sliaa(&c, 6, VCR_SLIAA_AT_BOX_FLAG, "4", "0", "1", "1", "1"), "flag first");
    CHECK(c.at_box && c.aa && vcr_sliaa_gate(&c) == 0, "flag position mattered");
    /* SLI only (cfg 5) reprograms the same scan-out clocks: NEW - refused
     * without the flag; OLD - sent */
    CHECK(!sliaa(&c, 5, "4", "1", "0", "0", "1"), "cfg 5 did not parse");
    CHECK(vcr_sliaa_gate(&c) != 0, "an SLI enable passed the gate with nobody at the box");
    CHECK(sliaa_gate_old(&c) == 0, "the old gate is not the one that shipped");
    CHECK(!sliaa(&c, 6, "4", "1", "0", "0", "1", VCR_SLIAA_AT_BOX_FLAG), "cfg 5 + flag");
    CHECK(vcr_sliaa_gate(&c) == 0, "cfg 5 with a person at the box refused");
    /* the way back needs no flag */
    CHECK(!sliaa(&c, 1, "off"), "off did not parse");
    CHECK(c.off && vcr_sliaa_gate(&c) == 0, "off refused");
    CHECK(!sliaa(&c, 2, "off", VCR_SLIAA_AT_BOX_FLAG), "off + flag");
    /* a shape with no video mux is refused by the TOOL, flag or not: a vendor
     * kernel has no refusal of its own (AmigaMerlin froze on cfg 1's) */
    CHECK(!sliaa(&c, 6, "4", "0", "1", "0", "1", VCR_SLIAA_AT_BOX_FLAG), "cfg 1 as sent");
    CHECK(vcr_sliaa_gate(&c) != 0, "cfg 1's {4,0,1,0,1} passed the tool");
    CHECK(sliaa_gate_old(&c) == 0, "the old gate sent it");
    CHECK(!sliaa(&c, 6, "2", "1", "1", "1", "0", VCR_SLIAA_AT_BOX_FLAG), "2-chip SLI + 4-sample");
    CHECK(vcr_sliaa_gate(&c) != 0, "{2,1,1,1,0} passed the tool");
    CHECK(!sliaa(&c, 6, "1", "1", "0", "0", "0", VCR_SLIAA_AT_BOX_FLAG), "1-chip SLI");
    CHECK(vcr_sliaa_gate(&c) != 0, "1-chip SLI passed the tool");
    /* ... and exactly the shapes the kernel accepts pass it */
    {
        static const char *const ns[3] = { "1", "2", "4" };
        static const char *const d[3] = { "0", "1", "2" };
        int ni, sl, aa, hi, an;
        for (ni = 0; ni < 3; ni++)
            for (sl = 0; sl < 2; sl++)
                for (aa = 0; aa < 2; aa++)
                    for (hi = 0; hi < 3; hi++)
                        for (an = 0; an < 2; an++) {
                            if (!sl && !aa)
                                continue;
                            CHECK(!sliaa(&c, 6, ns[ni], d[sl], d[aa], d[hi], d[an],
                                         VCR_SLIAA_AT_BOX_FLAG), "a shape did not parse");
                            CHECK_EQ_I(vcr_sliaa_gate(&c) == 0,
                                       vcr_sli_combo_ok(c.n, c.sli, c.aa, c.high, c.analog));
                        }
    }
    /* AA on a desktop PLL: refused unless the desktop is in 2x mode or the
     * operator forces it; an unreadable vidProcCfg counts as not 2x. SLI only
     * and `off` never ask. */
    CHECK(!sliaa(&c, 6, "4", "0", "1", "1", "1", VCR_SLIAA_AT_BOX_FLAG), "cfg 7");
    CHECK(vcr_sliaa_pll_gate(&c, 1) == 0, "cfg 7 on a 2x desktop refused");
    CHECK(vcr_sliaa_pll_gate(&c, 0) != 0, "cfg 7 on a desktop PLL sent");
    CHECK(vcr_sliaa_pll_gate(&c, VCR_SLIAA_2X_UNKNOWN) != 0, "cfg 7 with vidProcCfg unknown sent");
    CHECK(!sliaa(&c, 7, "4", "0", "1", "1", "1", VCR_SLIAA_AT_BOX_FLAG, VCR_SLIAA_FORCE_PLL),
          "cfg 7 forced");
    CHECK(c.force_pll && vcr_sliaa_pll_gate(&c, 0) == 0, "--force-desktop-pll did not force it");
    CHECK(vcr_sliaa_pll_gate(&c, VCR_SLIAA_2X_UNKNOWN) == 0, "--force-desktop-pll, unknown");
    CHECK(!sliaa(&c, 6, "4", "1", "0", "0", "1", VCR_SLIAA_AT_BOX_FLAG), "cfg 5");
    CHECK(vcr_sliaa_pll_gate(&c, 0) == 0, "an SLI-only enable asked about the PLL");
    CHECK(!sliaa(&c, 1, "off"), "off");
    CHECK(vcr_sliaa_pll_gate(&c, 0) == 0, "off asked about the PLL");
    CHECK(sliaa(&c, 6, "4", "0", "1", "1", "1", "--force-desktop-pl") != 0, "a truncated force flag");
    /* a near-miss flag is not the flag */
    CHECK(sliaa(&c, 6, "4", "0", "1", "1", "1", "--i-am-at-the-bo") != 0, "a truncated flag accepted");
    CHECK(sliaa(&c, 6, "4", "0", "1", "1", "1", "--at-box") != 0, "an unknown option accepted");
    CHECK(sliaa(&c, 6, "4", "0", "1", "1", "1", "-i-am-at-the-box") != 0, "a one-dash flag accepted");
    /* nothing loose: every value decimal or 0x-hex, in range, the right count */
    CHECK(sliaa(&c, 5, "-1", "1", "1", "0", "1") != 0, "-1 accepted");
    CHECK(sliaa(&c, 4, "4", "1", "1", "0") != 0, "4 values accepted");
    CHECK(sliaa(&c, 5, "3", "1", "0", "0", "1") != 0, "3 chips accepted");
    CHECK(sliaa(&c, 5, "4", "2", "0", "0", "1") != 0, "sli 2 accepted");
    CHECK(sliaa(&c, 5, "4", "0", "2", "0", "1") != 0, "aa 2 accepted (it would skip the gate's meaning)");
    CHECK(sliaa(&c, 5, "4", "0", "1", "3", "1") != 0, "16-sample accepted");
    CHECK(sliaa(&c, 5, "4", "1", "0", "0", "1x") != 0, "trailing garbage accepted");
    CHECK(sliaa(&c, 5, "4", "1", "0", "0", "") != 0, "empty value accepted");
    CHECK(sliaa(&c, 6, "4", "1", "0", "0", "1", "0x") != 0, "bare 0x accepted");
    CHECK(sliaa(&c, 6, "4", "1", "0", "0", "1", "0x123456789") != 0, "33-bit hex accepted");
    CHECK(sliaa(&c, 6, "4", "1", "0", "0", "1", "4294967296") != 0, "33-bit decimal accepted");
    CHECK(!sliaa(&c, 6, "4", "1", "0", "0", "1", "4294967295"), "0xffffffff refused");
    CHECK(sliaa(&c, 12, "4", "1", "0", "0", "1", "8", "16", "0", "0", "0", "0", "9") != 0,
          "12 values accepted");
    CHECK(sliaa(&c, 2, "off", "4") != 0, "off with a value accepted");
    CHECK(sliaa(&c, 0) != 0, "nothing accepted");

    /* Glide's request, field by field (minihwc.c HWC_MINIVDD_HACK) */
    CHECK(!sliaa(&c, 12, "4", "0", "1", "1", "1", "8", "16", "0x01b7e000", "0", "0x01000000",
                 "0x01180000", VCR_SLIAA_AT_BOX_FLAG), "the full form did not parse");
    memset(&r, 0xa5, sizeof r);
    vcr_sliaa_fill(&c, 4, 32u << 20, &r);
    CHECK_EQ_U(r.ChipInfo.dwChips, 4);
    CHECK_EQ_U(r.ChipInfo.dwsliEn, 0);
    CHECK_EQ_U(r.ChipInfo.dwaaEn, 1);
    CHECK_EQ_U(r.ChipInfo.dwaaSampleHigh, 1);
    CHECK_EQ_U(r.ChipInfo.dwsliAaAnalog, 1);
    CHECK_EQ_U(r.ChipInfo.dwsli_nlines, 8);
    CHECK_EQ_U(r.ChipInfo.dwCfgSwapAlgorithm, 1);           /* Glide always sends 1 */
    CHECK_EQ_U(r.MemInfo.dwTotalMemory, 32u << 20);         /* h3Mem * 1 MB */
    CHECK_EQ_U(r.MemInfo.dwTileMark, 0x01b7e000u);
    CHECK_EQ_U(r.MemInfo.dwTileCmpMark, 0x01b7e000u);       /* both colBuffStart0[0] */
    CHECK_EQ_U(r.MemInfo.dwaaSecondaryColorBufBegin, 0);
    CHECK_EQ_U(r.MemInfo.dwaaSecondaryDepthBufBegin, 0x01000000u);
    CHECK_EQ_U(r.MemInfo.dwaaSecondaryDepthBufEnd, 0x01180000u);
    CHECK_EQ_U(r.MemInfo.dwBpp, 16);
    CHECK_EQ_U(vcr_sli_req_tuple(&r), 0x40111u);
    /* the kernel reads it the way it reads Glide's: the table's cfg 7 */
    CHECK_EQ_I(vcr_sli_policy(&r, 1), 0);
    CHECK_EQ_I(vcr_sli_policy(&r, 0), VCR_SLI_R_AA_OFF);
    /* the short form: defaults */
    CHECK(!sliaa(&c, 5, "4", "1", "0", "0", "1"), "short form");
    vcr_sliaa_fill(&c, 4, 32u << 20, &r);
    CHECK_EQ_U(r.ChipInfo.dwsli_nlines, VCR_SLIAA_DEFAULT_NLINES);
    CHECK_EQ_U(r.MemInfo.dwBpp, VCR_SLIAA_DEFAULT_BPP);
    CHECK_EQ_U(r.MemInfo.dwTileMark, 0);
    /* totalMemory in whole MB, as Glide sends it (h3Mem = fbRam >> 20, then
     * * 1 MB) - not the raw byte count the escape reported */
    vcr_sliaa_fill(&c, 4, (32u << 20) + 0x1000u, &r);
    CHECK_EQ_U(r.MemInfo.dwTotalMemory, 32u << 20);
    CHECK(r.MemInfo.dwTotalMemory != (32u << 20) + 0x1000u, "totalMemory is fbRam, not h3Mem MB");
    /* off: Glide's disable, dwChips = the board's, nothing else */
    CHECK(!sliaa(&c, 1, "off"), "off");
    memset(&r, 0xa5, sizeof r);
    vcr_sliaa_fill(&c, 4, 32u << 20, &r);
    CHECK_EQ_U(r.ChipInfo.dwChips, 4);
    CHECK_EQ_U(r.ChipInfo.dwsliEn | r.ChipInfo.dwaaEn | r.MemInfo.dwBpp | r.MemInfo.dwTileMark, 0);
    CHECK_EQ_I(vcr_sli_policy(&r, 0), 0);                   /* a disable always goes through */
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
    RUN(the_decode_index_is_twice_the_memory_as_a_cfgPciDecode_index);
    RUN(an_explicit_32mb_chip_maps_exactly_as_the_legacy_default);
    RUN(a_64mb_chip_decodes_128mb_and_the_slaves_sit_128mb_above_the_master);
    RUN(a_decode_narrowed_for_32mb_is_widened_for_a_64mb_chip);
    RUN(a_memory_size_the_decode_cannot_express_is_refused_with_nothing_written);
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
    RUN(accepted_requests_write_exactly_what_they_wrote_before_the_safety_net);
    RUN(cfg1_as_glide_sends_it_is_refused_with_nothing_on_the_bus);
    RUN(the_combination_predicate_is_the_video_mux_branch_table);
    RUN(sample_count_is_ignored_without_aa_and_out_of_range_with_it);
    RUN(the_kill_switch_refuses_every_aa_request_and_nothing_else);
    RUN(the_persisted_phase_keeps_the_warn_mask_the_clock_result_and_the_refusal);
    RUN(glide_may_not_write_the_sli_aa_registers_behind_the_kernel);
    RUN(an_idle_slave_keeps_its_syncs_tristated_whatever_a_pci_op_writes);
    RUN(cfg0_close_pci_op_zeros_reach_the_chips_as_before);
    RUN(single_chip_aa_pci_op_writes_need_diag_sliaa_even_with_allow_poke);
    RUN(a0_read_toggles_in_a_live_sli_session_go_through);
    RUN(pci_op_offsets_outside_the_config_header_are_refused);
    RUN(a_non_vsa100_master_is_refused_with_nothing_written);
    RUN(the_vendor_memory_refusal_is_policy_before_any_teardown);
    RUN(the_aa_base_is_a_byte_address_in_the_vendor_recipe_only);
    RUN(a_real_aa_base_cannot_spill_in_the_vendor_recipe);
    RUN(the_aa_lfb_control_that_froze_aa_and_its_fix_match_silicon);
    RUN(one_sample_per_chip_is_exactly_the_three_paired_shapes);
    RUN(the_vendor_recipe_leaves_sli_only_requests_and_the_disable_alone);
    RUN(the_vendor_recipe_refuses_memory_info_it_cannot_place);
    RUN(aa_config_space_matches_the_expected_tables_for_both_recipes);
    RUN(cfg1_is_refused_by_both_recipes);
    RUN(the_aa_state_is_read_back_by_config_cycles_only);
    RUN(no_state_record_without_an_aa_enable);
    RUN(the_recipe_is_visible_in_the_persisted_phases);
    RUN(the_cfg3_arms_leave_every_other_shape_and_the_disable_alone);
    RUN(the_cfg3_arms_are_visible_in_the_persisted_phases);
    RUN(no_arm_can_reach_the_vsync_offset_that_froze_the_board);
    RUN(vcrctl_sliaa_refuses_every_enable_without_a_person_at_the_box);
    RUN(step_codes_are_unique);
)
