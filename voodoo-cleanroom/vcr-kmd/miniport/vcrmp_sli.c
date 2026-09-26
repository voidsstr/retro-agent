/*
 * vcrmp_sli.c - VSA-100 multi-chip (SLI / AA) bring-up for vcr-kmd.
 *
 * A port of 3dfx's own sequence from the Glide GPL source,
 * glide3x/h5/minihwc/dos_mode.c (cited below as D:<line>):
 *   mapSlavePhysical   D:227-323  -> vcr_sli_map_slaves()
 *   initSlave          D:326-390  -> init_slave()      (+ h3cinit.c h3InitResetAll,
 *                                                        h3InitVga; cited C:<line>)
 *   buildVideoModeData D:432-479  -> build_mode()
 *   setVideoModeSlave  D:481-589  -> set_mode_slave()
 *   hwcSetSLIAAMode    D:591-1482 -> sli_enable(), D:1483-1512 -> sli_disable()
 * plus the 3D sliCtrl value of glide3/src/gsst.c _grEnableSliCtrl /
 * _grDisableSliCtrl (G:3940-4028), which dos_mode.c leaves to Glide's command
 * FIFO: the kernel writes it too, because it has to clear it itself when a
 * Glide client dies without its teardown.
 * Licence: 3DFX GLIDE Source Code General Public License, like our Glide fork.
 *
 * Pure C over the accessor table in include/vcr_sli.h - no OS headers, no
 * floating point (the original has none in these functions either), no CRT.
 * The kernel supplies the table (raw PCI cycles for the slaves, the kernel
 * mapping of each chip's memBase0, the shared I/O BAR); the host test supplies
 * a mock of four chips (tests/native/test_vcr_kmd_sli.c).
 *
 * Rules kept here:
 *   - EVERY register write is logged through io->log BEFORE it is issued, so
 *     the flight recorder's last entry after a hang is the write that hung.
 *     Step codes: VCR_SLI_STEP_TABLE in vcr_sli.h.
 *   - every poll loop is bounded (VCR_SLI_ROOM_POLLS); an expired wait skips
 *     the write it guarded and says so (VCR_SLI_W_TIMEOUT), and never leaves
 *     the shared I/O BAR decoded by a slave.
 *   - a request is fully validated before the first write; a refusal writes
 *     nothing.
 *
 * Deliberate differences from dos_mode.c (each also marked where it happens):
 *   1. CHECKFORROOM (h3cinitdd.h:69) spins forever; ours is bounded.
 *   2. h3InitResetAll is handed slaveIOBase - the slave's memBase0 MEMORY
 *      address - and the DOS build's macros truncate it to a 16-bit I/O port
 *      (D:369, h3cinitdd.h:53). The target is unambiguously the slave, so the
 *      reset goes to the slave's own memBase0. Likewise every 32-bit register
 *      access of the ported cinit/setmode code uses the chip's memBase0 (the
 *      same registers the I/O BAR shows); only 8-bit VGA port accesses use the
 *      shared I/O BAR, through io->vga_rd/vga_wr, under the original's own
 *      I/O-decode toggling.
 *   3. The reset pulse is held 10 us (the original's back-to-back DOS port
 *      writes take ~1 us each; MMIO writes are posted).
 *   4. mapSlavePhysical's chip-count guess (D:246-260) is dead code - it
 *      compares a decode INDEX to 256 MB and divides it by 32 MB, and the
 *      result is never used - and is not ported.
 *   5. Where the original reads an uninitialised variable (sliBandHeight or
 *      bpp outside its switch, D:685-714, D:862-869) the request is refused.
 *   6. The swap-algorithm bit follows ChipInfo.dwCfgSwapAlgorithm (Glide
 *      always sends 1; D:776 hard-codes it).
 *   7. The V5 6000 external clock (D:617-621, gpio_6k_clock) is NOT ported:
 *      gpio.c has no licence header. vcr_sli_6k_clock() is the hook.
 *   9. initSlave's miscInit1 copy keeps the slave's own strap bits 24-28:
 *      dos_mode.c overwrites them with the master's, and the vendor driver
 *      leaves chips 2 and 3 with bit 28 set where chip 1 has it clear (golden
 *      sli_amigamerlin-3.1-r11_cfg5: 0x36008101 vs 0x26008101).
 *  10. The slaves also get the master's vidDesktopStartAddr, and
 *      vidPixelBufThold 0x10410 - neither is in dos_mode.c; both are what the
 *      vendor driver's slaves carry (goldens cfg5 and cfg5_1600).
 *  11. SLI disable clears only the SLI/AA fields of cfgSliAAMisc (vsync
 *      offset, slave wait), not the undocumented power-up bit 11 (0x800).
 *   8. After placing a slave's BARs, its BAR writes are turned off again.
 *      dos_mode.c leaves them on; the vendor driver does not (golden
 *      sli_amigamerlin-3.1-r11_cfg5_192.168.1.124.json: slave cfgInitEnable
 *      0x4ba07b01 in SLI, 0x301 outside it). With that one step the port
 *      reproduces the vendor's 4-chip SLI config space on .124 exactly
 *      (tests/native/test_vcr_kmd_sli.c).
 */
#include "../include/vcr_types.h"
#include "../include/vcr_regs.h"
#include "../include/vcr_sli.h"

#define MB(x)   ((vcr_u32)(x) << 20)

/* ---- access helpers: every write is logged, then issued ------------------ */

static void lg(const vcr_sli_io *io, vcr_u32 step, vcr_u32 chip, vcr_u32 reg,
               vcr_u32 val, const char *what)
{
    if (io->log)
        io->log(io->ctx, step, chip, reg, val, what);
}

static void stall(const vcr_sli_io *io, vcr_u32 us)
{
    if (io->stall_us)
        io->stall_us(io->ctx, us);
}

static vcr_u32 cfg_r(const vcr_sli_io *io, vcr_u32 chip, vcr_u32 off)
{
    return io->cfg_rd(io->ctx, chip, off);
}

static void cfg_w(const vcr_sli_io *io, vcr_u32 step, vcr_u32 chip, vcr_u32 off,
                  vcr_u32 v, const char *what)
{
    lg(io, step, chip, off, v, what);
    io->cfg_wr(io->ctx, chip, off, v);
}

static vcr_u32 reg_r(const vcr_sli_io *io, vcr_u32 chip, vcr_u32 off)
{
    return io->io_rd(io->ctx, chip, off);
}

static void reg_w(const vcr_sli_io *io, vcr_u32 step, vcr_u32 chip, vcr_u32 off,
                  vcr_u32 v, const char *what)
{
    lg(io, step, chip, off, v, what);
    io->io_wr(io->ctx, chip, off, v);
}

static vcr_u8 vga_r(const vcr_sli_io *io, vcr_u32 chip, vcr_u32 port)
{
    return io->vga_rd(io->ctx, chip, port);
}

/* ISET8PHYS */
static void vga_w(const vcr_sli_io *io, vcr_u32 step, vcr_u32 chip, vcr_u32 port,
                  vcr_u32 v, const char *what)
{
    lg(io, step, chip, port, v & 0xff, what);
    io->vga_wr(io->ctx, chip, port, (vcr_u8)v);
}

/* ISET16PHYS(port, (v << 8) | idx): index then data, logged as one step with
 * reg = VCR_SLI_VGA_IDX(port, idx) (bit 24 set, so index 0 is not mistaken
 * for a plain port write). */
static void vga_iw(const vcr_sli_io *io, vcr_u32 step, vcr_u32 chip, vcr_u32 port,
                   vcr_u32 idx, vcr_u32 v, const char *what)
{
    lg(io, step, chip, VCR_SLI_VGA_IDX(port, idx & 0xff), v & 0xff, what);
    io->vga_wr(io->ctx, chip, port, (vcr_u8)idx);
    io->vga_wr(io->ctx, chip, port + 1, (vcr_u8)v);
}

static int refuse(const vcr_sli_io *io, vcr_u32 reason, vcr_u32 val)
{
    lg(io, VCR_SLI_S_REFUSED, 0, reason, val, "request refused, nothing written");
    return reason == VCR_SLI_R_NODEV ? VCR_SLI_ENODEV : VCR_SLI_EINVAL;
}

static int io_ok(const vcr_sli_io *io)
{
    return io && io->cfg_rd && io->cfg_wr && io->io_rd && io->io_wr &&
           io->vga_rd && io->vga_wr;
}

static int chips_ok(vcr_u32 n)
{
    return n == 1 || n == 2 || n == 4;
}

static int is_vsa100(const vcr_sli_io *io, vcr_u32 chip)
{
    vcr_u32 id = cfg_r(io, chip, VCR_SLI_PCI_ID);
    return (id & 0xffff) == VCR_PCI_VENDOR_3DFX && VCR_IS_NAPALM(id >> 16);
}

/* Glide's realNumChips (minihwc.c:1795-1812): chip 1 answers -> two; chip 3
 * answers too -> four. */
static vcr_u32 real_chips(const vcr_sli_io *io)
{
    if (!is_vsa100(io, 1))
        return 1;
    return is_vsa100(io, 3) ? 4 : 2;
}

static int slaves_present(const vcr_sli_io *io, vcr_u32 n)
{
    vcr_u32 c;
    for (c = 1; c < n; c++)
        if (!is_vsa100(io, c))
            return (int)c;
    return 0;
}

/* CHECKFORROOM (h3cinitdd.h:69) - `while (!(_inp(regBase) & 0x3f))` - but
 * BOUNDED. Returns 1 when there is room, 0 (logged) when the wait expired. */
static int wait_room(const vcr_sli_io *io, vcr_u32 chip, vcr_u32 mask, const char *what)
{
    vcr_u32 i, st = 0;
    for (i = 0; i < VCR_SLI_ROOM_POLLS; i++) {
        st = reg_r(io, chip, VCR_R_STATUS);
        if (st & mask)
            return 1;
        stall(io, VCR_SLI_POLL_US);
    }
    lg(io, VCR_SLI_S_TIMEOUT, chip, VCR_R_STATUS, st, what);
    return 0;
}

/* A direct write to a 3D register goes through the chip's PCI FIFO; into a
 * full FIFO (a wedged engine) it would be retried on the bus for ever. */
static int write_3d(const vcr_sli_io *io, vcr_u32 step, vcr_u32 chip, vcr_u32 off,
                    vcr_u32 v, const char *what)
{
    if (!wait_room(io, chip, VCR_STATUS_PCIFIFO_FREE, what))
        return VCR_SLI_W_TIMEOUT;
    reg_w(io, step, chip, off, v, what);
    return 0;
}

/* Command-register I/O decode (bit 0). All chips share ONE I/O BAR (D:289-290:
 * "Slaves share IO space with the master, but have I/O cycles turned off"), so
 * the chip that answers is the chip whose I/O decode is on. */
static void iodec(const vcr_sli_io *io, vcr_u32 chip, int on, const char *what)
{
    vcr_u32 cmd = cfg_r(io, chip, VCR_SLI_PCI_COMMAND);
    cmd = on ? (cmd | VCR_SLI_CMD_IO) : (cmd & ~VCR_SLI_CMD_IO);
    cfg_w(io, on ? VCR_SLI_S_IODEC_ON : VCR_SLI_S_IODEC_OFF, chip, VCR_SLI_PCI_COMMAND,
          cmd, what);
}

/* D:359-366 / D:634-641: master off FIRST, then the slave on - so the shared
 * I/O BAR is never decoded by two chips at once. */
static void io_to_slave(const vcr_sli_io *io, vcr_u32 chip)
{
    iodec(io, 0, 0, "disable master IO");
    iodec(io, chip, 1, "enable slave IO");
}

/* D:374-381 / D:645-652: slave off first, then the master back on. */
static void io_to_master(const vcr_sli_io *io, vcr_u32 chip)
{
    iodec(io, chip, 0, "disable slave IO");
    iodec(io, 0, 1, "enable master IO");
}

/* ---- mapSlavePhysical (D:227-323) ------------------------------------------ */

/* D:204-225 memDecode[], in MB */
static const vcr_u16 k_mem_decode_mb[16] = {
    128, 256, 512, 1024,  64, 32, 16, 8,  4, 0, 0, 0,  0, 0, 0, 0
};

int vcr_sli_map_slaves(const vcr_sli_io *io, vcr_u32 nchips,
                       vcr_u32 slave_bar0[4], vcr_u32 slave_bar1[4])
{
    vcr_u32 dec, dec0, m0, m1, mio, mb1_idx, mb1_bytes, c, v;
    int warn = 0, bad;

    if (!io_ok(io))
        return io ? refuse(io, VCR_SLI_R_ACCESSORS, 0) : VCR_SLI_EINVAL;
    if (!slave_bar0 || !slave_bar1)
        return refuse(io, VCR_SLI_R_ACCESSORS, 1);
    if (!chips_ok(nchips))
        return refuse(io, VCR_SLI_R_CHIPS, nchips);
    if ((bad = slaves_present(io, nchips)) != 0)
        return refuse(io, VCR_SLI_R_NODEV, (vcr_u32)bad);

    /* D:237-239 First, make sure the master has been reconfigured to only
     * decode 32MB for memBase0, 64MB for memBase1, and 256B for ioBase0. */
    dec0 = dec = cfg_r(io, 0, VCR_CFG_PCIDECODE);
    /* D:241-243 If memBase0 is not set to 32MB then the board is still in its
     * powerup config and we need to fiddle with it. (D:246-260, a chip-count
     * guess whose result is never used, is not ported - see the header.) */
    if ((dec & VCR_PCIDEC_MB0_MASK) != VCR_PCIDEC_32MB) {
        /* D:263-266 remap master to make room for slaves */
        dec &= ~(VCR_PCIDEC_MB0_MASK | VCR_PCIDEC_MB1_MASK | VCR_PCIDEC_IO_MASK);
        dec |= VCR_PCIDEC_32MB | VCR_PCIDEC_IO_256 |
               (VCR_PCIDEC_64MB << VCR_PCIDEC_MB1_SHIFT);
    }

    /* D:270-273 the master's physical addresses. Narrowing the decode only
     * shrinks the BAR size masks; the assigned (larger-aligned) base stays. */
    m0  = cfg_r(io, 0, VCR_SLI_PCI_BAR0) & ~0xfu;
    m1  = cfg_r(io, 0, VCR_SLI_PCI_BAR1) & ~0xfu;
    mio = cfg_r(io, 0, VCR_SLI_PCI_IOBAR) & ~0xfu;

    /* D:281-283 how much memory space the master uses for memBase1 */
    mb1_idx = (dec & VCR_PCIDEC_MB1_MASK) >> VCR_PCIDEC_MB1_SHIFT;
    mb1_bytes = MB(k_mem_decode_mb[mb1_idx]);

    /* ours: refuse BARs the arithmetic below would wrap or zero */
    if (!m0 || !m1 || !mb1_bytes || m0 + MB(32) * (nchips - 1) < m0 ||
        m1 + mb1_bytes < m1)
        return refuse(io, VCR_SLI_R_BARS, m0);

    lg(io, VCR_SLI_S_MAP_BEGIN, 0, dec0, nchips, "mapSlavePhysical");
    if (dec != dec0)
        cfg_w(io, VCR_SLI_S_MAP_MASTER_DEC, 0, VCR_CFG_PCIDECODE, dec,
              "master decode 32 MB / 64 MB / 256 B (was power-up)");

    for (c = 1; c < nchips; c++) {
        /* D:285-290 Calculate desired slave addresses. All slaves share the
         * same memBase1 address space. Slaves share IO space with the master,
         * but have I/O cycles turned off. */
        vcr_u32 s0 = m0 + MB(32) * c;
        vcr_u32 s1 = m1 + mb1_bytes;
        vcr_u32 sdec;

        /* D:296-300 Enable init register writes */
        cfg_w(io, VCR_SLI_S_MAP_INITEN, c, VCR_CFG_INITENABLE,
              (VCR_IE_HW_INIT_WRITES | VCR_IE_PCI_FIFO_WRITES | VCR_IE_BASE_ADDR_WRITES) << 8,
              "cfgInitEnable: init, PCI FIFO and BAR writes on");

        /* D:302-308 Make sure slaves are only going to decode what we want
         * them to. The slaves always decode the same amount of memory that the
         * master does, but some of the slave memory areas overlap. Make sure
         * we set up the right snoop enable stuff too. */
        sdec = dec & ~(VCR_PCIDEC_SNOOP_MB0_MASK | VCR_PCIDEC_SNOOP_MB1_MASK);
        sdec |= VCR_PCIDEC_32MB << VCR_PCIDEC_SNOOP_MB0_SHIFT;
        sdec |= mb1_idx << VCR_PCIDEC_SNOOP_MB1_SHIFT;
        cfg_w(io, VCR_SLI_S_MAP_DECODE, c, VCR_CFG_PCIDECODE, sdec, "slave cfgPciDecode");

        /* D:310-313 Program slaves for their new home. */
        cfg_w(io, VCR_SLI_S_MAP_BAR0, c, VCR_SLI_PCI_BAR0, s0, "slave memBase0");
        cfg_w(io, VCR_SLI_S_MAP_BAR1, c, VCR_SLI_PCI_BAR1, s1, "slave memBase1");
        cfg_w(io, VCR_SLI_S_MAP_IOBAR, c, VCR_SLI_PCI_IOBAR, mio, "slave ioBase = master's");

        /* D:315-319 Enable memory decode on slave, but keep IO disabled. */
        v = cfg_r(io, c, VCR_SLI_PCI_COMMAND);
        v = (v & ~VCR_SLI_CMD_IO) | VCR_SLI_CMD_MEM;
        cfg_w(io, VCR_SLI_S_MAP_COMMAND, c, VCR_SLI_PCI_COMMAND, v,
              "slave command: memory on, IO off");

        /* ours (header difference 8): BAR writes off again, the vendor's state */
        v = (cfg_r(io, c, VCR_CFG_INITENABLE) >> 8) & ~VCR_IE_BASE_ADDR_WRITES;
        cfg_w(io, VCR_SLI_S_MAP_LOCK, c, VCR_CFG_INITENABLE, v << 8,
              "cfgInitEnable: BAR writes off again");

        /* ours: report what the chip now decodes, not what we asked for */
        slave_bar0[c] = cfg_r(io, c, VCR_SLI_PCI_BAR0) & ~0xfu;
        slave_bar1[c] = cfg_r(io, c, VCR_SLI_PCI_BAR1) & ~0xfu;
        lg(io, VCR_SLI_S_MAP_DONE, c, slave_bar0[c], slave_bar1[c], "slave BARs read back");
        if (slave_bar0[c] != s0 || slave_bar1[c] != s1)
            warn |= VCR_SLI_W_READBACK;
    }
    slave_bar0[0] = m0;
    slave_bar1[0] = m1;
    return warn;
}

/* ---- initSlave (D:326-390) ------------------------------------------------- */

/* h3InitResetAll (C:1317-1342), aimed at the slave's memBase0 - see header 2. */
static void reset_all(const vcr_sli_io *io, vcr_u32 c)
{
    const vcr_u32 rst = VCR_MI0_GRX_RESET | VCR_MI0_FBI_FIFO_RESET | VCR_MI0_VIDEO_RESET |
                        VCR_MI0_2D_RESET | VCR_MI0_MEMORY_TIMING_RESET |
                        VCR_MI0_VGA_TIMING_RESET;
    vcr_u32 mi0 = reg_r(io, c, VCR_R_MISCINIT0);
    vcr_u32 mi1 = reg_r(io, c, VCR_R_MISCINIT1);

    reg_w(io, VCR_SLI_S_INIT_RESET, c, VCR_R_MISCINIT1, mi1 | VCR_MI1_CMDSTREAM_RESET,
          "command stream reset on");
    reg_w(io, VCR_SLI_S_INIT_RESET, c, VCR_R_MISCINIT0, mi0 | rst,
          "grx, FIFO, video, 2D, memory and VGA timing reset on");
    stall(io, 10);                              /* header difference 3 */
    reg_w(io, VCR_SLI_S_INIT_RESET, c, VCR_R_MISCINIT0, mi0 & ~rst, "resets off");
    reg_w(io, VCR_SLI_S_INIT_RESET, c, VCR_R_MISCINIT1, mi1 & ~VCR_MI1_CMDSTREAM_RESET,
          "command stream reset off");
}

/* h3InitVga(regBase, legacyDecode) (C:710-748); each CHECKFORROOM bounded. */
static int init_vga(const vcr_sli_io *io, vcr_u32 c, int legacy_decode)
{
    vcr_u32 v = VCR_VGA0_EXTENSIONS | VCR_VGA0_WAKEUP_3C3 |
                (legacy_decode ? 0 : VCR_VGA0_LEGACY_DECODE);   /* alt readback = 0 */
    int warn = 0;

    if (wait_room(io, c, VCR_STATUS_ROOM_MASK, "room for vgaInit0"))
        reg_w(io, VCR_SLI_S_INIT_VGA, c, VCR_R_VGAINIT0, v,
              "vgaInit0: extensions, wake-up at 3C3, legacy decode off");
    else
        warn |= VCR_SLI_W_TIMEOUT;
    /* C:731-733 disable VESA mode and VGA VIDEO lock bits */
    if (wait_room(io, c, VCR_STATUS_ROOM_MASK, "room for vgaInit1"))
        reg_w(io, VCR_SLI_S_INIT_VGA, c, VCR_R_VGAINIT1, 0, "vgaInit1: VESA/lock bits off");
    else
        warn |= VCR_SLI_W_TIMEOUT;
    if (wait_room(io, c, VCR_STATUS_ROOM_MASK, "room for VGA wake-up"))
        vga_w(io, VCR_SLI_S_INIT_VGA, c, VCR_VGA_WAKEUP, 0x01, "VGA wake-up");
    else
        warn |= VCR_SLI_W_TIMEOUT;
    /* C:738-742 CLUT invert address */
    v = reg_r(io, c, VCR_R_MISCINIT1) | VCR_MI1_CLUT_INVERT;
    if (wait_room(io, c, VCR_STATUS_ROOM_MASK, "room for miscInit1"))
        reg_w(io, VCR_SLI_S_INIT_VGA, c, VCR_R_MISCINIT1, v, "miscInit1: CLUT invert address");
    else
        warn |= VCR_SLI_W_TIMEOUT;
    return warn;
}

static const struct { vcr_u16 off; const char *name; } k_init_copy1[] = {
    { VCR_R_PLLCTRL1,  "pllCtrl1 (core clock)" },
    { VCR_R_DRAMINIT0, "dramInit0" },
    { VCR_R_DRAMINIT1, "dramInit1" },
    { VCR_R_PCIINIT0,  "pciInit0" },
};

static int init_slave(const vcr_sli_io *io, vcr_u32 c)
{
    vcr_u32 i, v;
    int warn;

    lg(io, VCR_SLI_S_INIT_BEGIN, c, 0, 0, "initSlave");

    /* D:337-345 Copy over pll & memory timings, etc. */
    for (i = 0; i < sizeof k_init_copy1 / sizeof k_init_copy1[0]; i++)
        reg_w(io, VCR_SLI_S_INIT_COPY, c, k_init_copy1[i].off,
              reg_r(io, 0, k_init_copy1[i].off), k_init_copy1[i].name);
    /* D:346-349 miscInit0 without the raw-LFB byte swizzle, master AND slave */
    v = reg_r(io, 0, VCR_R_MISCINIT0) & ~VCR_MI0_RAWLFB_BYTE_SWIZZLE;
    reg_w(io, VCR_SLI_S_INIT_MISC0, 0, VCR_R_MISCINIT0, v, "master miscInit0");
    reg_w(io, VCR_SLI_S_INIT_MISC0, c, VCR_R_MISCINIT0, v, "slave miscInit0");
    /* D:350-353 - but the slave keeps its OWN strap bits 24-28 (PCI fast
     * device, BIOS size, 66 MHz, AGP, device type): dos_mode.c copies the
     * master's whole word, the vendor driver does not (header difference 9). */
    v = (reg_r(io, 0, VCR_R_MISCINIT1) & ~VCR_MI1_STRAPS) |
        (reg_r(io, c, VCR_R_MISCINIT1) & VCR_MI1_STRAPS);
    reg_w(io, VCR_SLI_S_INIT_COPY, c, VCR_R_MISCINIT1, v, "miscInit1 (slave straps kept)");
    reg_w(io, VCR_SLI_S_INIT_COPY, c, VCR_R_TMUGBEINIT, reg_r(io, 0, VCR_R_TMUGBEINIT),
          "tmuGbeInit");

    /* D:355-357 Init DRAM mode stuff */
    reg_w(io, VCR_SLI_S_INIT_DRAM, c, VCR_R_DRAMDATA, 0x00000037, "dramData");
    reg_w(io, VCR_SLI_S_INIT_DRAM, c, VCR_R_DRAMCOMMAND, 0x10d, "dramCommand");

    /* D:359-366 Disable master IO, enable slave IO */
    io_to_slave(io, c);
    /* D:368-369 Reset everything */
    reset_all(io, c);
    /* D:371-372 Init VGA (no legacy decode) */
    warn = init_vga(io, c, 0);
    /* D:374-381 Disable slave IO, enable master IO - even after a timeout */
    io_to_master(io, c);

    /* D:383-389 */
    lg(io, VCR_SLI_S_INIT_READBACK, c, VCR_R_STATUS, reg_r(io, c, VCR_R_STATUS), "slave status");
    lg(io, VCR_SLI_S_INIT_READBACK, c, VCR_R_VGAINIT0, reg_r(io, c, VCR_R_VGAINIT0),
       "slave vgaInit0");
    lg(io, VCR_SLI_S_INIT_READBACK, c, VCR_R_VGAINIT1, reg_r(io, c, VCR_R_VGAINIT1),
       "slave vgaInit1");
    return warn;
}

int vcr_sli_init_slaves(const vcr_sli_io *io, vcr_u32 nchips)
{
    vcr_u32 c;
    int warn = 0, bad;
    if (!io_ok(io))
        return io ? refuse(io, VCR_SLI_R_ACCESSORS, 0) : VCR_SLI_EINVAL;
    if (!chips_ok(nchips))
        return refuse(io, VCR_SLI_R_CHIPS, nchips);
    if ((bad = slaves_present(io, nchips)) != 0)
        return refuse(io, VCR_SLI_R_NODEV, (vcr_u32)bad);
    for (c = 1; c < nchips; c++)
        warn |= init_slave(io, c);
    return warn;
}

/* ---- buildVideoModeData / setVideoModeSlave (D:392-589) ------------------- */

#define MD_N 21
/* D:402-425 modeData[] slots 0-15 are these CRTC indices */
static const vcr_u8 k_crtc_idx[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x09, 0x10, 0x11, 0x12, 0x15, 0x16, 0x1a, 0x1b
};
static const char *const k_md_name[MD_N] = {
    "CR00 htotal", "CR01 hdisp end", "CR02 hblank start", "CR03 hblank end",
    "CR04 hsync start", "CR05 hsync end", "CR06 vtotal", "CR07 overflow",
    "CR09 max scan line", "CR10 vsync start", "CR11 vsync end", "CR12 vdisp end",
    "CR15 vblank start", "CR16 vblank end", "CR1A h ext", "CR1B v ext",
    "misc output", "SR01", "pllCtrl0 low", "pllCtrl0 high", "dacMode (2X)"
};
/* D:392-396 */
static const vcr_u8 k_vgaattr[20] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x0f, 0x00
};

/* D:432-479 Snarf all VGA data we need from the master (its I/O decode is on) */
static void build_mode(const vcr_sli_io *io, vcr_u32 md[MD_N])
{
    vcr_u32 i, pll;
    for (i = 0; i < 16; i++) {
        vga_w(io, VCR_SLI_S_MODE_INDEX, 0, VCR_VGA_CRTC_I, k_crtc_idx[i], "CRTC index");
        md[i] = vga_r(io, 0, VCR_VGA_CRTC_D);
    }
    md[16] = vga_r(io, 0, VCR_VGA_MISC_R);       /* written to 0x3c2, read from 0x3cc */
    vga_w(io, VCR_SLI_S_MODE_INDEX, 0, VCR_VGA_SEQ_I, 0x01, "SEQ index");
    md[17] = vga_r(io, 0, VCR_VGA_SEQ_D);
    pll = reg_r(io, 0, VCR_R_PLLCTRL0);
    md[18] = pll & 0xff;
    md[19] = (pll >> 8) & 0xff;
    md[20] = reg_r(io, 0, VCR_R_DACMODE) & 0xffff;   /* (FxU16) - see report on DPMS bits */
    for (i = 0; i < MD_N; i++)
        lg(io, VCR_SLI_S_MODE_CAPTURED, 0, i, md[i], k_md_name[i]);
}

/* D:481-589, with the slave's I/O decode on (the caller toggles it). */
static void set_mode_slave(const vcr_sli_io *io, vcr_u32 c, const vcr_u32 md[MD_N])
{
    vcr_u32 i, v;

    /* D:490-501 MISC REGISTERS. This register gets programmed first since the
     * Mono/Color selection needs to be made. Sync polarities. Also force the
     * programmable clock to be used with bits 3&2. */
    vga_w(io, VCR_SLI_S_MODE_VGA, c, VCR_VGA_MISC_W, md[16] | 0x01, "misc output (color I/O)");

    /* D:503-525 CRTC REGISTERS. First Unlock CRTC, then program them.
     * ("Mystical VGA Magic": a 16-bit 0x0011 to 3D4 = CR11 <- 0, which clears
     * the CR0-7 write protect.) */
    vga_iw(io, VCR_SLI_S_MODE_VGA, c, VCR_VGA_CRTC_I, 0x11, 0x00, "CR11 = 0: unlock CR0-7");
    for (i = 0; i < 16; i++)
        vga_iw(io, VCR_SLI_S_MODE_VGA, c, VCR_VGA_CRTC_I, k_crtc_idx[i], md[i], k_md_name[i]);

    /* D:527-530 Enable Sync Outputs */
    vga_iw(io, VCR_SLI_S_MODE_VGA, c, VCR_VGA_CRTC_I, 0x17, 0x80, "CR17: sync outputs on");

    /* D:532-537 VIDCLK (32 bit access only!) Set the Video Clock to the
     * correct frequency */
    reg_w(io, VCR_SLI_S_MODE_REG, c, VCR_R_PLLCTRL0, (md[19] << 8) | md[18], "pllCtrl0");

    /* D:539-543 dacMode (32 bit access only!) (sets up 1x mode or 2x mode) */
    reg_w(io, VCR_SLI_S_MODE_REG, c, VCR_R_DACMODE, md[20], "dacMode");

    /* D:545-553 the 1x / 2x bit must also be set in vidProcConfig to properly
     * enable 1x / 2x mode */
    v = reg_r(io, c, VCR_R_VIDPROCCFG) & ~(VCR_VPC_2X_MODE_EN | VCR_VPC_HALF_MODE);
    if (md[20])
        v |= VCR_VPC_2X_MODE_EN;
    reg_w(io, VCR_SLI_S_MODE_REG, c, VCR_R_VIDPROCCFG, v, "vidProcCfg 1x/2x");

    /* D:555-562 SEQ REGISTERS: set run mode in the sequencer (not reset);
     * make sure bit 5 == 0 (i.e., screen on) */
    vga_iw(io, VCR_SLI_S_MODE_VGA, c, VCR_VGA_SEQ_I, 0x01, md[17] & ~0x20u, "SR01, screen on");
    vga_iw(io, VCR_SLI_S_MODE_VGA, c, VCR_VGA_SEQ_I, 0x00, 0x03, "SR00: run");

    /* D:564-570 turn off VGA's screen refresh, as this function only sets
     * extended video modes, and the VGA screen refresh eats up performance
     * (10% difference in screen to screen blits!). */
    reg_w(io, VCR_SLI_S_MODE_REG, c, VCR_R_VGAINIT0,
          reg_r(io, c, VCR_R_VGAINIT0) | VCR_VGA0_EXTSHIFTOUT, "vgaInit0: VGA refresh off");

    /* D:572-585 Make sure attribute index register is initialized */
    (void)vga_r(io, c, VCR_VGA_IS1_R);
    for (i = 0; i < 20; i++) {
        vga_w(io, VCR_SLI_S_MODE_VGA, c, VCR_VGA_ATTR_W, i, "ATTR index");
        vga_w(io, VCR_SLI_S_MODE_VGA, c, VCR_VGA_ATTR_W, k_vgaattr[i], "ATTR data");
    }
    vga_w(io, VCR_SLI_S_MODE_VGA, c, VCR_VGA_ATTR_W, 0x34, "ATTR: palette on, index 0x14");
    vga_w(io, VCR_SLI_S_MODE_VGA, c, VCR_VGA_IS1_R, 0x00, "3DA <- 0");
}

/* D:654-681 Copy over video processor config registers */
static void copy_video(const vcr_sli_io *io, vcr_u32 c)
{
    static const struct { vcr_u16 off; const char *name; } k_copy[] = {
        { VCR_R_VIDOVERLAYSTARTCOORDS, "vidOverlayStartCoords" },
        { VCR_R_VIDOVERLAYENDSCREENCOORD, "vidOverlayEndScreenCoord" },
        { VCR_R_VIDOVERLAYDUDXOFFSETSRCWIDTH, "vidOverlayDudxOffsetSrcWidth" },
        { VCR_R_VIDDESKTOPOVERLAYSTRIDE, "vidDesktopOverlayStride" },
        { VCR_R_VIDOVERLAYDUDX, "vidOverlayDudx" },
        { VCR_R_VIDMAXRGBDELTA, "vidMaxRGBDelta" },
        /* ours (header difference 10): the vendor's slaves carry it too */
        { VCR_R_VIDDESKTOPSTARTADDR, "vidDesktopStartAddr" },
    };
    vcr_u32 i, v;

    reg_w(io, VCR_SLI_S_MODE_COPY, c, VCR_R_VIDSCREENSIZE,
          reg_r(io, 0, VCR_R_VIDSCREENSIZE), "vidScreenSize");
    /* D:658-661 Make sure video processor is reset so the size change takes
     * effect. */
    v = reg_r(io, 0, VCR_R_VIDPROCCFG);
    reg_w(io, VCR_SLI_S_MODE_COPY, c, VCR_R_VIDPROCCFG, v & ~VCR_VPC_VIDEO_PROCESSOR_EN,
          "vidProcCfg, video processor off");
    reg_w(io, VCR_SLI_S_MODE_COPY, c, VCR_R_VIDPROCCFG, v | VCR_VPC_VIDEO_PROCESSOR_EN,
          "vidProcCfg, video processor on");
    /* D:663-664 Note: We don't set any start addresses here because it's done
     * as part of the 3D init stuff... */
    for (i = 0; i < sizeof k_copy / sizeof k_copy[0]; i++)
        reg_w(io, VCR_SLI_S_MODE_COPY, c, k_copy[i].off, reg_r(io, 0, k_copy[i].off),
              k_copy[i].name);
    /* ours (header difference 10): NOT the master's - by now that is Glide's
     * own value (0x20820 at 1600x1200); the vendor's slaves hold 0x10410 at
     * 640x480 and at 1600x1200 alike (goldens cfg5, cfg5_1600) */
    reg_w(io, VCR_SLI_S_MODE_COPY, c, VCR_R_VIDPIXELBUFTHOLD, VCR_SLI_SLAVE_PIXBUFTHOLD,
          "vidPixelBufThold (vendor slave value)");
}

/* ---- hwcSetSLIAAMode enable (D:591-1482) ----------------------------------- */

typedef struct sli_p {
    vcr_u32 n, sli, aa, high, analog, nlines, lb, nlog2, bpp, swap;
    vcr_u32 col, dbeg, dend, mem0, mem1;
} sli_p;

static vcr_u32 log2_lines(vcr_u32 nlines)       /* D:684-700; 0 = invalid */
{
    switch (nlines) {
    case 2: return 1;   case 4: return 2;   case 8: return 3;   case 16: return 4;
    case 32: return 5;  case 64: return 6;  case 128: return 7;
    }
    return 0;
}

/* The CFG_VIDEOCTRL0/1/2 macros (D:911-926) */
static void vc0(const vcr_sli_io *io, vcr_u32 c, vcr_u32 flags, vcr_u32 mux_t, vcr_u32 mux_f)
{
    cfg_w(io, VCR_SLI_S_VIDEOCTRL0, c, VCR_CFG_VIDEOCTRL0,
          flags | (mux_t << VCR_VC0_OTHERMUX_TRUE_SHIFT) | (mux_f << VCR_VC0_OTHERMUX_FALSE_SHIFT),
          "cfgVideoCtrl0");
}
static void vc1(const vcr_sli_io *io, vcr_u32 c, vcr_u32 render_fetch, vcr_u32 compare_fetch,
                vcr_u32 render_crt, vcr_u32 compare_crt)
{
    cfg_w(io, VCR_SLI_S_VIDEOCTRL1, c, VCR_CFG_VIDEOCTRL1,
          (render_fetch << VCR_VC1_RENDER_FETCH_SHIFT) |
          (compare_fetch << VCR_VC1_COMPARE_FETCH_SHIFT) |
          (render_crt << VCR_VC1_RENDER_CRT_SHIFT) |
          (compare_crt << VCR_VC1_COMPARE_CRT_SHIFT), "cfgVideoCtrl1");
}
static void vc2(const vcr_sli_io *io, vcr_u32 c, vcr_u32 render, vcr_u32 compare)
{
    cfg_w(io, VCR_SLI_S_VIDEOCTRL2, c, VCR_CFG_VIDEOCTRL2,
          (render << VCR_VC2_RENDER_AAFIFO_SHIFT) | (compare << VCR_VC2_COMPARE_AAFIFO_SHIFT),
          "cfgVideoCtrl2");
}

#define EN      VCR_VC0_ENHANCED_VIDEO_EN
#define SLV     VCR_VC0_ENHANCED_VIDEO_SLV
#define LMUX    VCR_VC0_LOCALMUX_DESKTOP_PLUS_OVERLAY
#define HTRI    VCR_VC0_DAC_HSYNC_TRISTATE
#define CINV    VCR_VC0_SLI_AAFIFO_COMPARE_INV
#define DIV1    VCR_VC0_DIVIDE_BY_1
#define DIV2    VCR_VC0_DIVIDE_BY_2
#define DIV4    VCR_VC0_DIVIDE_BY_4
#define DIV8    VCR_VC0_DIVIDE_BY_8
#define PIPE    VCR_VC0_MUX_PIPE
#define PAA     VCR_VC0_MUX_PIPE_PLUS_AAFIFO
#define AAF     VCR_VC0_MUX_AAFIFO

/* D:928-1428, the video mux per SLI/AA combination, branch for branch.
 * Returns 0, or VCR_SLI_W_NOMUX when no branch matches (the original then
 * leaves cfgVideoCtrl0/1/2 as they were). */
static int video_mux(const vcr_sli_io *io, const sli_p *p, vcr_u32 c, vcr_u32 vid2x)
{
    const vcr_u32 n = p->n, sli = p->sli, aa = p->aa, high = p->high, analog = p->analog;
    const vcr_u32 L = p->lb, all = (n - 1) << L, me = c << L;
    const vcr_u32 aafifo_cmp = vid2x ? 0xff : 0x00;

    if (n == 1 && aa) {
        /* D:928-939 Single chip, 2-sample AA */
        vc0(io, c, EN | LMUX | DIV2, 0, PIPE);
        vc1(io, c, 0, 0, 0, 0);
        vc2(io, c, 0x00, 0xff);
    } else if (n == 2 && !sli && aa && high && !analog) {
        /* D:940-967 Two chips, 4-sample digital AA */
        if (c == 0) {
            vc0(io, c, EN | LMUX | DIV4, PAA, 0);
            vc1(io, c, 0, 0, 0, 0);
            vc2(io, c, 0, 0);
        } else {
            vc0(io, c, EN | SLV | LMUX | DIV1, PIPE, 0);
            vc1(io, c, 0, 0, 0, 0xff);
            vc2(io, c, 0, 0);
        }
    } else if (n == 2 && !sli && aa && high && analog) {
        /* D:968-991 Two chips, 4-sample analog AA */
        if (c == 0)
            vc0(io, c, EN | LMUX | DIV4, PIPE, 0);
        else
            vc0(io, c, EN | SLV | HTRI | LMUX | DIV4, PIPE, 0);
        vc1(io, c, 0, 0, 0, 0);
        vc2(io, c, 0, 0);
    } else if (n == 2 && sli && !aa && !analog) {
        /* D:992-1018 Two chips, 2-way digital SLI */
        if (c == 0) {
            vc0(io, c, EN | DIV1, AAF, PIPE);
            vc1(io, c, 0x01 << L, 0, 0, 0);
            vc2(io, c, 0x01 << L, 0x01 << L);
        } else {
            vc0(io, c, EN | SLV | DIV1, PIPE, PIPE);
            vc1(io, c, all, me, 0, 0xff);
            vc2(io, c, all, me);
        }
    } else if ((n == 2 || n == 4) && sli && !aa && analog) {
        /* D:1019-1044 2 or 4 chips, 2/4-way analog SLI */
        if (c == 0) {
            vc0(io, c, EN | DIV1, PIPE, PIPE);
            vc1(io, c, all, 0, all, 0);
            vc2(io, c, 0x00, 0xff);
        } else {
            vc0(io, c, EN | SLV | DIV1, PIPE, PIPE);
            vc1(io, c, all, me, all, me);
            vc2(io, c, 0x00, 0xff);
        }
    } else if (n == 2 && sli && aa && !high && !analog) {
        /* D:1045-1072 Two chips, 2-sample AA with 2-way digital SLI */
        if (c == 0) {
            vc0(io, c, EN | LMUX | DIV2, AAF, PIPE);
            vc1(io, c, 0x01 << L, 0, 0, 0);
            vc2(io, c, 0x01 << L, 0x01 << L);
        } else {
            vc0(io, c, EN | SLV | LMUX | DIV1, PIPE, PIPE);
            vc1(io, c, all, me, 0, 0xff);
            vc2(io, c, all, me);
        }
    } else if (n == 2 && sli && aa && !high && analog) {
        /* D:1073-1100 Two chips, 2-sample AA with 2-way analog SLI */
        if (c == 0) {
            vc0(io, c, EN | LMUX | DIV2, 0, PIPE);
            vc1(io, c, 0x01 << L, 0, 0x01 << L, 0);
            vc2(io, c, 0x00, 0xff);
        } else {
            vc0(io, c, EN | SLV | LMUX | DIV2, 0, PIPE);
            vc1(io, c, all, me, all, me);
            vc2(io, c, 0x00, 0xff);
        }
    } else if (n == 4 && sli && aa && !high && analog) {
        /* D:1101-1139 Four chip, 2-sample AA. two units of 2 chip analog SLI,
         * with 1 subsample per unit. */
        if (c == 0) {
            vc0(io, c, EN | DIV2, PAA, 0);
            vc1(io, c, 0x01 << L, 0x00 << L, 0x01 << L, 0x00 << L);
            vc2(io, c, 0, 0);
        } else if (c == 1 || c == 3) {
            vc0(io, c, EN | SLV | HTRI | DIV1, PIPE, 0);
            vc1(io, c, 0x01 << L, (c >> 1) << L, 0x00 << L, 0xffu << L);
            vc2(io, c, 0, 0);
        } else {
            vc0(io, c, EN | SLV | DIV2, 0, PAA);
            vc1(io, c, 0x01 << L, 0x01 << L, 0x01 << L, 0x01 << L);
            vc2(io, c, 0x00, 0xff);
        }
    } else if (n == 4 && !sli && aa && high == 1 && analog) {
        /* D:1140-1187 Four chip, 4-sample AA. 1 subsample per chip analog
         * SLI'ed. (lin_mode.c:908 tests `sliEnable` here, which makes the
         * branch unreachable; dos_mode.c's `!sliEnable` is the consistent one.) */
        if (c == 0) {
            vc0(io, c, EN | DIV4, PAA, 0);
            vc1(io, c, 0, 0, 0, 0);
            vc2(io, c, 0x00, aafifo_cmp);
        } else if (c == 1 || c == 3) {
            vc0(io, c, EN | SLV | HTRI | DIV1, PIPE, 0);
            vc1(io, c, 0, 0, 0, 0xff);
            vc2(io, c, 0x00, aafifo_cmp);
        } else {
            vc0(io, c, EN | SLV | HTRI | DIV4, 0, PAA);
            vc1(io, c, 0, 0, 0, 0);
            vc2(io, c, 0x00, 0xff);
        }
    } else if (n == 4 && !sli && aa && high == 2 && analog) {
        /* D:1188-1238 Four chip, 8-sample AA. 2 subsample per chip analog SLI'ed */
        if (c == 0) {
            vc0(io, c, EN | LMUX | DIV8, PAA, 0);
            vc1(io, c, 0, 0, 0, 0);
            vc2(io, c, 0x00, aafifo_cmp);
        } else if (c == 1 || c == 3) {
            vc0(io, c, EN | SLV | LMUX | HTRI | DIV1, PIPE, 0);
            vc1(io, c, 0, 0, 0, 0xff);
            vc2(io, c, 0x00, aafifo_cmp);
        } else {
            vc0(io, c, EN | SLV | LMUX | HTRI | DIV8, 0, PAA);
            vc1(io, c, 0, 0, 0, 0);
            vc2(io, c, 0x00, 0xff);
        }
    } else if (n == 2 && !sli && aa && !high && !analog) {
        /* D:1239-1264 Two chips, 2-sample AA. 1 subsample per chip digital SLI'ed */
        if (c == 0) {
            vc0(io, c, EN | DIV2, PAA, 0);
            vc1(io, c, 0, 0, 0, 0);
            vc2(io, c, 0, 0);
        } else {
            vc0(io, c, EN | SLV | DIV1, PIPE, 0);
            vc1(io, c, 0, 0, 0, 0xff);
            vc2(io, c, 0, 0);
        }
    } else if (n == 2 && !sli && aa && !high && analog) {
        /* D:1265-1286 Two chips, 2-sample AA. 1 subsample per chip analog SLI'ed */
        if (c == 0)
            vc0(io, c, EN | DIV2, PIPE, 0);
        else
            vc0(io, c, EN | SLV | HTRI | DIV2, PIPE, 0);
        vc1(io, c, 0, 0, 0, 0);
        vc2(io, c, 0, 0);
    } else if (n == 4 && sli && !aa && !analog) {
        /* D:1287-1314 Four chips, 4-way digital SLI */
        if (c == 0) {
            vc0(io, c, EN | CINV | DIV1, AAF, PIPE);
            vc1(io, c, all, 0, 0, 0);
            vc2(io, c, all, 0x00 << L);
        } else {
            vc0(io, c, EN | SLV | DIV1, PIPE, PIPE);
            vc1(io, c, all, me, 0, 0xff);
            vc2(io, c, all, me);
        }
    } else if (n == 4 && sli && aa && !high && !analog) {
        /* D:1315-1343 Four chips, 2-sample AA with 4-way digital SLI */
        if (c == 0) {
            vc0(io, c, EN | LMUX | CINV | DIV2, AAF, PIPE);
            vc1(io, c, all, 0, 0, 0);
            vc2(io, c, all, 0x00);
        } else {
            vc0(io, c, EN | SLV | LMUX | DIV1, PIPE, PIPE);
            vc1(io, c, all, me, 0, 0xff);
            vc2(io, c, all, me);
        }
    } else if (n == 4 && sli && aa && high == 1 && !analog) {
        /* D:1344-1385 Four chip, 2-way analog SLI with digital 4-sample AA */
        if (c == 0) {
            vc0(io, c, EN | LMUX | DIV4, PAA, 0);
            vc1(io, c, 0x01 << L, 0x00 << L, 0x01 << L, 0x00 << L);
            vc2(io, c, 0, 0);
        } else if (c == 1 || c == 3) {
            vc0(io, c, EN | SLV | HTRI | LMUX | DIV1, PIPE, 0);
            vc1(io, c, 0x01 << L, (c >> 1) << L, 0x00 << L, 0xffu << L);
            vc2(io, c, 0, 0);
        } else {
            vc0(io, c, EN | SLV | LMUX | DIV4, 0, PAA);
            vc1(io, c, 0x01 << L, 0x01 << L, 0x01 << L, 0x01 << L);
            vc2(io, c, 0x00, 0xff);
        }
    } else if (n == 4 && sli && aa && high == 1 && analog) {
        /* D:1386-1428 Four chip, 2-way analog SLI with analog 4-sample AA */
        if (c == 0) {
            vc0(io, c, EN | LMUX | DIV4, PIPE, 0);
            vc1(io, c, 0x01 << L, 0x00 << L, 0x01 << L, 0x00 << L);
            vc2(io, c, 0, 0);
        } else if (c == 1 || c == 3) {
            vc0(io, c, EN | SLV | HTRI | LMUX | DIV4, PIPE, 0);
            vc1(io, c, 0x01 << L, (c >> 1) << L, 0x01 << L, (c >> 1) << L);
            vc2(io, c, 0, 0);
        } else {
            vc0(io, c, EN | SLV | LMUX | DIV4, PIPE, 0);
            vc1(io, c, 0x01 << L, 0x01 << L, 0x01 << L, 0x01 << L);
            vc2(io, c, 0, 0);
        }
    } else {
        lg(io, VCR_SLI_S_NOMUX, c, 0, (sli) | (aa << 1) | (analog << 2) | (high << 4),
           "no cfgVideoCtrl branch: left as it was");
        return VCR_SLI_W_NOMUX;
    }
    return 0;
}

/* D:725-1482, the body of the per-chip loop */
static int config_chip(const vcr_sli_io *io, const sli_p *p, vcr_u32 c)
{
    const vcr_u32 n = p->n, sli = p->sli, aa = p->aa, high = p->high, analog = p->analog;
    const vcr_u32 L = p->lb;
    vcr_u32 v, vid2x, render, compare, scan;
    int warn;

    /* D:726-728 Is 2x video mode enabled? */
    vid2x = reg_r(io, 0, VCR_R_VIDPROCCFG) & VCR_VPC_2X_MODE_EN;

    /* D:730-768 Set up pciInit0 and tmuGbeInit (same on master and slaves) */
    v = (reg_r(io, c, VCR_R_PCIINIT0) & ~(VCR_PI0_RETRY_INTERVAL | VCR_PI0_FORCE_FB_HIGH)) |
        VCR_PI0_READ_WS | VCR_PI0_WRITE_WS;
    reg_w(io, VCR_SLI_S_PCIINIT0, c, VCR_R_PCIINIT0, v, "pciInit0");
    v = (reg_r(io, c, VCR_R_TMUGBEINIT) & ~(VCR_TMU_AA_CLK_DELAY | VCR_TMU_AA_CLK_INVERT)) |
        (0x02u << VCR_TMU_AA_CLK_DELAY_SHIFT) | VCR_TMU_AA_CLK_INVERT;
    reg_w(io, VCR_SLI_S_TMUGBEINIT, c, VCR_R_TMUGBEINIT, v, "tmuGbeInit");

    /* D:770-804 Set up buffer swap control and snooping */
    if (n > 1) {
        /* Buffer swapping */
        v = cfg_r(io, c, VCR_CFG_INITENABLE) >> 8;
        v &= ~VCR_IE_SWAP_MASTER;
        v |= (p->swap ? VCR_IE_SWAPBUFFER_ALGORITHM : 0) | (c == 0 ? VCR_IE_SWAP_MASTER : 0);
        cfg_w(io, VCR_SLI_S_SWAP, c, VCR_CFG_INITENABLE, v << 8,
              c == 0 ? "cfgInitEnable: swap master" : "cfgInitEnable: swap slave");

        /* Enable snooping */
        if (c == 0) {
            v = cfg_r(io, c, VCR_CFG_INITENABLE) >> 8;
            v |= VCR_IE_ADDRESS_SNOOP;
            cfg_w(io, VCR_SLI_S_SNOOP, c, VCR_CFG_INITENABLE, v << 8,
                  "cfgInitEnable: master address snoop");
        } else {
            v = cfg_r(io, c, VCR_CFG_INITENABLE) >> 8;
            v &= ~VCR_IE_MEMBASE0_SNOOP;
            v |= VCR_IE_ADDRESS_SNOOP | VCR_IE_MEMBASE0_SNOOP_EN | VCR_IE_MEMBASE1_SNOOP_EN |
                 VCR_IE_ADDRESS_SNOOP_SLAVE | VCR_IE_INIT_REGISTER_SNOOP |
                 (((p->mem0 >> 22) & 0x3ff) << VCR_IE_MEMBASE0_SNOOP_SHIFT) |
                 (n > 2 ? VCR_IE_QUICK_SAMPLING : 0);
            cfg_w(io, VCR_SLI_S_SNOOP, c, VCR_CFG_INITENABLE, v << 8,
                  "cfgInitEnable: slave snoops the master's memBase0/1 and init registers");

            v = cfg_r(io, c, VCR_CFG_PCIDECODE);
            v &= ~VCR_PCIDEC_MB1_SNOOP;
            v |= ((p->mem1 >> 22) & 0x3ff) << VCR_PCIDEC_MB1_SNOOP_SHIFT;
            cfg_w(io, VCR_SLI_S_SNOOP_DECODE, c, VCR_CFG_PCIDECODE, v,
                  "cfgPciDecode: master memBase1 snoop address");
        }
    }

    /* D:806-854 cfgSliLfbCtrl */
    scan = p->nlines - 1;
    if (sli && (!aa || !high)) {
        if (n == 4 && aa && !high && analog) {
            /* 4 chip 2 sample AA. two 2-way SLI units with 1 subsample per SLI unit */
            render = ((n >> 1) - 1) << L;
            compare = (c >> 1) << L;
            v = (p->nlog2 - 1) << VCR_SLILFB_NUMCHIPS_LOG2_SHIFT;
        } else {
            /* No aa or 2 sample AA w/SLI */
            render = (n - 1) << L;
            compare = c << L;
            v = p->nlog2 << VCR_SLILFB_NUMCHIPS_LOG2_SHIFT;
        }
        v |= (render << VCR_SLILFB_RENDERMASK_SHIFT) | (compare << VCR_SLILFB_COMPAREMASK_SHIFT) |
             (scan << VCR_SLILFB_SCANMASK_SHIFT) | VCR_SLILFB_CPU_WRITE_EN |
             VCR_SLILFB_DISPATCH_WRITE_EN | VCR_SLILFB_READ_EN;
        cfg_w(io, VCR_SLI_S_SLILFBCTRL, c, VCR_CFG_SLILFBCTRL, v, "cfgSliLfbCtrl");
    } else if (!sli && aa) {
        /* SLI disabled, AA enabled */
        cfg_w(io, VCR_SLI_S_SLILFBCTRL, c, VCR_CFG_SLILFBCTRL, 0, "cfgSliLfbCtrl: AA only");
    } else {
        /* SLI enabled, AA enabled, 4 sample AA enabled */
        render = ((n >> 1) - 1) << L;
        compare = (c >> 1) << L;
        v = (render << VCR_SLILFB_RENDERMASK_SHIFT) | (compare << VCR_SLILFB_COMPAREMASK_SHIFT) |
            (scan << VCR_SLILFB_SCANMASK_SHIFT) |
            ((p->nlog2 - 1) << VCR_SLILFB_NUMCHIPS_LOG2_SHIFT) | VCR_SLILFB_CPU_WRITE_EN |
            VCR_SLILFB_DISPATCH_WRITE_EN | VCR_SLILFB_READ_EN;
        cfg_w(io, VCR_SLI_S_SLILFBCTRL, c, VCR_CFG_SLILFBCTRL, v, "cfgSliLfbCtrl: SLI + 4-sample AA");
    }

    /* D:856-881 cfgSliAATiledAperture: nothing for SLI alone; AA is enabled */
    if (!(sli && !aa)) {
        vcr_u32 fmt = p->bpp == 15 ? VCR_AALFB_FMT_15BPP
                    : p->bpp == 32 ? VCR_AALFB_FMT_32BPP : VCR_AALFB_FMT_16BPP;
        /* D:871 shifts a BYTE address left by 4; minihwc.c's single-chip AA
         * path (HWC_GDX_INIT, ~l.5464) ORs it in unshifted. UNVERIFIED which
         * one the hardware wants - ported as dos_mode.c has it. */
        v = (p->col << VCR_AALFB_SECONDARY_BASE_SHIFT) | VCR_AALFB_CPU_WRITE_EN |
            VCR_AALFB_DISPATCH_WRITE_EN | fmt | (high ? VCR_AALFB_RD_DIVIDE_BY_4 : 0);
        cfg_w(io, VCR_SLI_S_AALFBCTRL, c, VCR_CFG_AALFBCTRL, v, "cfgAALfbCtrl");
        v = ((p->dbeg >> 12) << VCR_AADEPTH_BEGIN_SHIFT) | ((p->dend >> 12) << VCR_AADEPTH_END_SHIFT);
        cfg_w(io, VCR_SLI_S_AADEPTH, c, VCR_CFG_AADEPTHBUFAPERTURE, v, "cfgAADepthBufferAperture");
    }

    /* D:883-909 Set up vga_vsync_offset field in cfgSliAAMisc */
    if (n > 1 && c > 0 && (aa || sli)) {
        vcr_u32 pixels = 7, chars, hxtra = 0;
        if ((analog &&
             !(n == 4 && sli && aa && !high && analog && c != 2) &&
             !(n == 4 && !sli && aa && high && analog && c != 2 && vid2x == 0)) ||
            (n == 4 && sli && aa && high && !analog && c == 2))
            chars = 4;      /* four chips, 2-way analog SLI with digital 4-sample AA... */
        else
            chars = 5;      /* Run slave 8 clocks ahead */
        v = cfg_r(io, c, VCR_CFG_SLIAAMISC) & ~VCR_SLIAA_VSYNC_OFFSET;
        v |= (pixels << VCR_SLIAA_VSYNC_PIXELS_SHIFT) | (chars << VCR_SLIAA_VSYNC_CHARS_SHIFT) |
             (hxtra << VCR_SLIAA_VSYNC_HXTRA_SHIFT);
        cfg_w(io, VCR_SLI_S_VSYNC_OFFSET, c, VCR_CFG_SLIAAMISC, v, "cfgSliAAMisc vsync offset");
    }

    /* D:911-1428 */
    warn = video_mux(io, p, c, vid2x);

    /* D:1430-1437 Make sure that last chip properly waits for data to be
     * xfered over the PCI bus before driving... */
    if ((n == 4 && sli && aa && high && c == 3) ||
        (n == 4 && sli && aa && !high && analog && c == 3)) {
        v = cfg_r(io, c, VCR_CFG_SLIAAMISC) | VCR_SLIAA_LFB_RD_SLV_WAIT;
        cfg_w(io, VCR_SLI_S_SLV_WAIT, c, VCR_CFG_SLIAAMISC, v, "cfgSliAAMisc: AA LFB read slave wait");
    }

    /* D:1439-1451 Deal with the problem for LFB reads where the data really
     * needs to come from 4 different chips. Since the hardware does not
     * support this, only aliased data is returned: the Master returns its lfb
     * data. By turning off AA LFB reads, chips 2/3 no longer snoop lfb reads
     * at all. Then, there is only handshaking between chips 0 & 1 so the
     * Master stays happy. */
    if (n == 4 && !sli && aa && high && analog && c > 1) {
        v = cfg_r(io, c, VCR_CFG_AALFBCTRL) & ~VCR_AALFB_READ_EN;
        cfg_w(io, VCR_SLI_S_AA_READ_OFF, c, VCR_CFG_AALFBCTRL, v, "cfgAALfbCtrl: AA LFB reads off");
    }

    if (c > 0) {
        /* D:1453-1463 For the slave chips, make the video PLL lock to the
         * Master's sync_clk_out clock output... and power down the slave's
         * RAMDAC. */
        v = cfg_r(io, c, VCR_CFG_VIDEOCTRL0) | VCR_VC0_VIDPLL_SEL;
        cfg_w(io, VCR_SLI_S_VIDPLL_SEL, c, VCR_CFG_VIDEOCTRL0, v, "video PLL from master sync_clk_out");
        v = reg_r(io, c, VCR_R_MISCINIT1) | VCR_MI1_POWERDOWN_DAC;
        reg_w(io, VCR_SLI_S_DAC_OFF, c, VCR_R_MISCINIT1, v, "slave RAMDAC powered down");
    } else if (n == 4) {
        /* D:1465-1471 Special Case 4 way where master also needs to sync from slave */
        v = cfg_r(io, c, VCR_CFG_VIDEOCTRL0) | VCR_VC0_VIDPLL_SEL;
        cfg_w(io, VCR_SLI_S_VIDPLL_SEL, c, VCR_CFG_VIDEOCTRL0, v, "4-way: master video PLL from slave");
    }
    return warn;
}

/* gsst.c _grEnableSliCtrl (G:3940-4003), without the Y-origin swap of the
 * compare mask (Glide re-writes sliCtrl through its FIFO with the swap). */
vcr_u32 vcr_sli_slictrl(vcr_u32 n, vcr_u32 nlines, vcr_u32 aa_en, vcr_u32 aa_sample_high,
                        vcr_u32 chip)
{
    vcr_u32 samples = !aa_en ? 1 : aa_sample_high == 0 ? 2 : aa_sample_high == 1 ? 4 : 8;
    vcr_u32 div, units, lb = log2_lines(nlines), l2c;

    if (n == 2)
        div = samples == 4 ? 2 : 1;
    else if (n == 4)
        div = samples == 2 ? 2 : 1;
    else
        return 0;               /* "should never happen" (G:3960) */
    units = n / div;
    if (units < 2 || !lb || chip >= n)
        return 0;
    l2c = units == 2 ? 1 : 2;
    return (((units - 1) << lb) << VCR_SLICTRL_RENDER_SHIFT) |
           (((chip / div) << lb) << VCR_SLICTRL_COMPARE_SHIFT) |
           (((1u << lb) - 1) << VCR_SLICTRL_SCAN_SHIFT) |
           (l2c << VCR_SLICTRL_LOG2_CHIPS_SHIFT) | VCR_SLICTRL_ENABLE;
}

static int sli_enable(const vcr_sli_io *io, const sli_p *p)
{
    vcr_u32 c, md[MD_N];
    int warn = 0, rc;

    lg(io, VCR_SLI_S_SET_BEGIN, p->n,
       p->sli | (p->aa << 1) | (p->analog << 2) | (p->high << 4), p->nlines,
       "hwcSetSLIAAMode: enable");

    /* D:617-621 v56k has an external clock! (Glide keys this on the REAL chip
     * count, not the requested one.) */
    if (real_chips(io) == 4) {
        rc = vcr_sli_6k_clock(io);
        lg(io, VCR_SLI_S_CLOCK_6K, 0, 0, (vcr_u32)rc,
           rc == VCR_SLI_OK ? "V5 6000 clock programmed" : "V5 6000 clock NOT programmed");
        if (rc != VCR_SLI_OK)
            warn |= VCR_SLI_W_NOCLOCK;
    }

    /* D:623-626 First, init all chips */
    for (c = 1; c < p->n; c++)
        warn |= init_slave(io, c);

    /* D:628-629 Grab video mode data from master. */
    build_mode(io, md);

    /* D:631-682 Now set up video modes */
    for (c = 1; c < p->n; c++) {
        io_to_slave(io, c);
        set_mode_slave(io, c, md);
        io_to_master(io, c);
        copy_video(io, c);
    }

    /* D:724-1482 Now on to the MUCH nastier backend stuff... */
    for (c = 0; c < p->n; c++)
        warn |= config_chip(io, p, c);

    /* G:3940-4003 sliCtrl, chip by chip. The master first: once snooping is
     * on, a write to the master's memBase0 also lands on every slave, so a
     * slave's own value must come after it. */
    if (p->sli)
        for (c = 0; c < p->n; c++) {
            vcr_u32 v = vcr_sli_slictrl(p->n, p->nlines, p->aa, p->high, c);
            if (v)
                warn |= write_3d(io, VCR_SLI_S_SLICTRL, c, VCR_3D_SLICTRL, v, "sliCtrl");
        }

    lg(io, VCR_SLI_S_SET_DONE, p->n, 0, (vcr_u32)warn, "hwcSetSLIAAMode: enable done");
    return warn;
}

/* ---- hwcSetSLIAAMode disable (D:1483-1512) --------------------------------- */

/* D:1484-1510, the per-chip half of the disable: everything but the 3D
 * sliCtrl. It is the whole of what the VIDEO path needs, and it is shared
 * with vcr_sli_reset_video() so the bugcheck path cannot drift from it. */
static int sli_disable_video(const vcr_sli_io *io, vcr_u32 n)
{
    vcr_u32 c, v;

    for (c = 0; c < n; c++) {
        v = cfg_r(io, c, VCR_CFG_INITENABLE) >> 8;
        v &= ~(VCR_IE_ADDRESS_SNOOP | VCR_IE_MEMBASE0_SNOOP_EN | VCR_IE_MEMBASE1_SNOOP_EN |
               VCR_IE_ADDRESS_SNOOP_SLAVE | VCR_IE_INIT_REGISTER_SNOOP | VCR_IE_MEMBASE0_SNOOP |
               VCR_IE_QUICK_SAMPLING | VCR_IE_SWAP_MASTER | VCR_IE_SWAPBUFFER_ALGORITHM);
        cfg_w(io, VCR_SLI_S_OFF_INITEN, c, VCR_CFG_INITENABLE, v << 8,
              "cfgInitEnable: snoop/swap off");

        cfg_w(io, VCR_SLI_S_OFF_CFG, c, VCR_CFG_SLILFBCTRL, 0, "cfgSliLfbCtrl = 0");
        cfg_w(io, VCR_SLI_S_OFF_CFG, c, VCR_CFG_AALFBCTRL, 0, "cfgAALfbCtrl = 0");
        /* ours (header difference 11): D:1493 writes 0, which also clears the
         * undocumented bit 11 every chip powers up with (0x800 on .124); only
         * the fields SLI/AA set are cleared here */
        v = cfg_r(io, c, VCR_CFG_SLIAAMISC) & ~(VCR_SLIAA_VSYNC_OFFSET | VCR_SLIAA_LFB_RD_SLV_WAIT);
        cfg_w(io, VCR_SLI_S_OFF_CFG, c, VCR_CFG_SLIAAMISC, v, "cfgSliAAMisc: SLI/AA fields cleared");
        /* D:1494 Make sure slave chips don't drive HSYNC & VSYNC */
        cfg_w(io, VCR_SLI_S_OFF_VIDEOCTRL0, c, VCR_CFG_VIDEOCTRL0,
              c > 0 ? (VCR_VC0_DAC_HSYNC_TRISTATE | VCR_VC0_DAC_VSYNC_TRISTATE) : 0,
              c > 0 ? "cfgVideoCtrl0: slave syncs tristated" : "cfgVideoCtrl0 = 0");
        cfg_w(io, VCR_SLI_S_OFF_CFG, c, VCR_CFG_VIDEOCTRL1, 0, "cfgVideoCtrl1 = 0");
        cfg_w(io, VCR_SLI_S_OFF_CFG, c, VCR_CFG_VIDEOCTRL2, 0, "cfgVideoCtrl2 = 0");
        cfg_w(io, VCR_SLI_S_OFF_CFG, c, VCR_CFG_AADEPTHBUFAPERTURE, 0, "cfgAADepthBufferAperture = 0");

        /* D:1504-1510 Kill the DAC & video output on the slaves */
        if (c > 0) {
            reg_w(io, VCR_SLI_S_OFF_DAC, c, VCR_R_DACMODE,
                  VCR_DAC_DPMS_ON_VSYNC | VCR_DAC_DPMS_ON_HSYNC, "slave dacMode: syncs off");
            v = reg_r(io, c, VCR_R_VIDPROCCFG) & ~VCR_VPC_VIDEO_PROCESSOR_EN;
            reg_w(io, VCR_SLI_S_OFF_VIDPROC, c, VCR_R_VIDPROCCFG, v, "slave video processor off");
        }
    }
    return 0;
}

static int sli_disable(const vcr_sli_io *io, vcr_u32 n)
{
    vcr_u32 c;
    int warn = 0;

    lg(io, VCR_SLI_S_OFF_BEGIN, n, 0, n, "hwcSetSLIAAMode: disable");

    /* G:4010-4028 _grDisableSliCtrl - while snooping is still on, so the
     * master's write reaches every slave even if a slave's own does not. */
    for (c = 0; c < n; c++)
        warn |= write_3d(io, VCR_SLI_S_OFF_SLICTRL, c, VCR_3D_SLICTRL, 0, "sliCtrl = 0");

    warn |= sli_disable_video(io, n);
    lg(io, VCR_SLI_S_OFF_DONE, n, 0, (vcr_u32)warn, "hwcSetSLIAAMode: disable done");
    return warn;
}

/* ---- the reset path: the video half of the disable, and nothing else --------
 * HwResetHw - a bugcheck's blue screen, or shutdown - hands the display back
 * to the HAL's text mode (vcrmp_hw.c VcrHwResetToVga). Under 4-way SLI the
 * master's cfgVideoCtrl0 has ENHANCED_VIDEO_EN and VIDPLL_SEL set
 * (D:1465-1471; 0x801 on .124, golden sli_*_cfg5): its video clock comes from
 * the slave side / the 6000's external synthesizer, not from its own
 * pllCtrl0, so restoring pllCtrl0 alone would leave the text screen scanning
 * at whatever clock that is - far under a CRT's 30 kHz floor. Single-chip AA
 * sets ENHANCED_VIDEO_EN with a video divide-by-2 (D:928-939), likely the
 * same kind of trap, and the same write undoes it. D:1498's 0 is what the
 * master carries outside SLI/AA (golden cfg0); D:1496 and D:1504-1510 stop
 * the slaves driving the syncs.
 * Only the per-chip half runs (sli_disable_video): the 3D sliCtrl writes wait
 * for PCI FIFO room a wedged engine never frees - bounded, but poll time at
 * HIGH_LEVEL for a register that only steers rendering. No request is
 * validated because there is none: it does what it can for n chips
 * (1..VCR_SLI_MAX_CHIPS). The caller supplies accessors legal at any IRQL -
 * no registry, no allocation, no HAL bus-data call. */
int vcr_sli_reset_video(const vcr_sli_io *io, vcr_u32 n)
{
    int warn;
    if (!io || !io->cfg_rd || !io->cfg_wr || !io->io_rd || !io->io_wr || n < 1 ||
        n > VCR_SLI_MAX_CHIPS)
        return VCR_SLI_EINVAL;
    lg(io, VCR_SLI_S_OFF_BEGIN, n, 0, n, "SLI/AA video path off for a reset (no 3D writes)");
    warn = sli_disable_video(io, n);
    lg(io, VCR_SLI_S_OFF_DONE, n, 0, (vcr_u32)warn, "SLI/AA video path off: done");
    return warn;
}

/* ---- entry point --------------------------------------------------------------- */

int vcr_sli_set(const vcr_sli_io *io, const vcr_sli_aa_req *r)
{
    sli_p p;
    int bad;

    if (!io_ok(io))
        return io ? refuse(io, VCR_SLI_R_ACCESSORS, 0) : VCR_SLI_EINVAL;
    if (!r)
        return refuse(io, VCR_SLI_R_ACCESSORS, 2);

    p.n = r->ChipInfo.dwChips;
    if (!chips_ok(p.n))
        return refuse(io, VCR_SLI_R_CHIPS, p.n);
    if ((bad = slaves_present(io, p.n)) != 0)
        return refuse(io, VCR_SLI_R_NODEV, (vcr_u32)bad);

    p.sli = r->ChipInfo.dwsliEn ? 1 : 0;
    p.aa = r->ChipInfo.dwaaEn ? 1 : 0;
    /* A disable request carries garbage in everything but dwChips. */
    if (!p.sli && !p.aa)
        return sli_disable(io, p.n);

    p.analog = r->ChipInfo.dwsliAaAnalog ? 1 : 0;
    p.high = p.aa ? r->ChipInfo.dwaaSampleHigh : 0;   /* only ever read together with aa */
    p.nlines = r->ChipInfo.dwsli_nlines;
    p.lb = log2_lines(p.nlines);
    p.nlog2 = p.n == 4 ? 2 : p.n == 2 ? 1 : 0;           /* D:704-714 */
    p.swap = r->ChipInfo.dwCfgSwapAlgorithm ? 1 : 0;
    p.bpp = r->MemInfo.dwBpp;
    p.col = r->MemInfo.dwaaSecondaryColorBufBegin;
    p.dbeg = r->MemInfo.dwaaSecondaryDepthBufBegin;
    p.dend = r->MemInfo.dwaaSecondaryDepthBufEnd;

    if (p.high > 2)
        return refuse(io, VCR_SLI_R_SAMPLE, p.high);
    if (p.sli && p.n < 2)
        return refuse(io, VCR_SLI_R_SLI_1CHIP, p.n);
    if (p.sli && !p.lb)
        return refuse(io, VCR_SLI_R_NLINES, p.nlines);
    if (p.aa && p.bpp != 15 && p.bpp != 16 && p.bpp != 32)
        return refuse(io, VCR_SLI_R_BPP, p.bpp);
    if (!p.lb)
        p.nlines = 1;           /* AA only: band height unused (scan mask 0) */

    /* D:718-719 */
    p.mem0 = cfg_r(io, 0, VCR_SLI_PCI_BAR0) & ~0xfu;
    p.mem1 = cfg_r(io, 0, VCR_SLI_PCI_BAR1) & ~0xfu;
    lg(io, VCR_SLI_S_SET_MEMINFO, 0, r->MemInfo.dwTotalMemory, r->MemInfo.dwTileMark,
       "totalMem / tileMark: unused, as in dos_mode.c");
    return sli_enable(io, &p);
}

/* =============================================================================
 * V5 6000 EXTERNAL CLOCK - PLACEHOLDER, NOT IMPLEMENTED.
 * dos_mode.c:617-621 calls gpio_6k_clock() for a 4-chip board before any
 * slave is started; the clock is programmed through GPIO in the board's HiNT
 * HB1 PCI bridge (3388:0021, bus 2 dev 0 on .124, config register 0xC4).
 * Glide's gpio.c has no licence header and is NOT ported. Our implementation
 * replaces this stub (or defines VCR_SLI_HAVE_6K_CLOCK and links its own).
 * =========================================================================== */
#ifndef VCR_SLI_HAVE_6K_CLOCK
int vcr_sli_6k_clock(const vcr_sli_io *io)
{
    (void)io;
    return VCR_SLI_ENOTIMPL;
}
#endif
