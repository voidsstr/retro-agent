/*
 * vcrmp.c - vcr-kmd video miniport: entry points and the IOCTL dispatcher.
 *
 * Our own XP miniport for the 3dfx Banshee / Voodoo 3 / VSA-100 family
 * (first target: the Voodoo 5 6000 on .124), plus a QEMU std-vga backend
 * that proves the driver chassis in a VM. See ../README.md.
 *
 * Safety model, because a display driver that fails at boot takes the box
 * off the network (docs/ and vcrmp_log.c):
 *   - `<service>\Diag\Disable` = 1 declines the adapter outright;
 *   - `BootAttempts` is incremented and FLUSHED before any hardware access
 *     and cleared by the stable-boot timer after StableSeconds; past
 *     MaxBootAttempts the adapter is declined and XP boots on its VGA driver;
 *   - every phase is persisted, so a hang that needs a power cycle still says
 *     where it stopped.
 */
#include "vcrmp.h"

#define NT_CURRENT_PROCESS ((PVOID)(LONG_PTR)-1)

static VP_STATUS NTAPI VcrFindAdapter(PVOID, PVOID, PWSTR, PVIDEO_PORT_CONFIG_INFO, PUCHAR);
static BOOLEAN   NTAPI VcrInitialize(PVOID);
static BOOLEAN   NTAPI VcrStartIO(PVOID, PVIDEO_REQUEST_PACKET);
static BOOLEAN   NTAPI VcrResetHw(PVOID, ULONG, ULONG);
static VOID      NTAPI VcrTimer(PVOID);
static VP_STATUS NTAPI VcrGetPowerState(PVOID, ULONG, PVIDEO_POWER_MANAGEMENT);
static VP_STATUS NTAPI VcrSetPowerState(PVOID, ULONG, PVIDEO_POWER_MANAGEMENT);
static VP_STATUS NTAPI VcrGetChildDescriptor(PVOID, PVIDEO_CHILD_ENUM_INFO,
                                             PVIDEO_CHILD_TYPE, PUCHAR, PULONG, PULONG);

ULONG NTAPI DriverEntry(PVOID Context1, PVOID Context2)
{
    VIDEO_HW_INITIALIZATION_DATA d;
    ULONG st, boots;

    VcrLogCreate((PUNICODE_STRING)Context2);
    boots = VcrDiagGet(L"BootCount", 0) + 1;
    VcrDiagSet(L"BootCount", boots, FALSE);

    VideoPortZeroMemory(&d, sizeof d);
    d.HwInitDataSize = sizeof d;
    d.AdapterInterfaceType = PCIBus;
    d.HwFindAdapter = VcrFindAdapter;
    d.HwInitialize = VcrInitialize;
    d.HwStartIO = VcrStartIO;
    d.HwResetHw = VcrResetHw;
    d.HwTimer = VcrTimer;
    d.HwGetPowerState = VcrGetPowerState;
    d.HwSetPowerState = VcrSetPowerState;
    d.HwGetVideoChildDescriptor = VcrGetChildDescriptor;
    d.HwDeviceExtensionSize = sizeof(VCR_EXT);

    st = VideoPortInitialize(Context1, Context2, &d, NULL);
    VLOG(VCR_LV_INFO, VCR_EV_DRIVER_ENTRY, st, boots, VCR_KMD_BUILD, sizeof(VCR_EXT),
         "vcrmp %u.%u build %u: VideoPortInitialize %x", VCR_KMD_VERSION_MAJOR,
         VCR_KMD_VERSION_MINOR, VCR_KMD_BUILD, st);
    return st;
}

/* ---- find ----------------------------------------------------------------------- */

static void hwinfo(VCR_EXT *x)
{
    static const WCHAR chip_v5[] = L"3dfx VSA-100 (vcr-kmd)";
    static const WCHAR chip_v3[] = L"3dfx Voodoo 3 / Banshee (vcr-kmd)";
    static const WCHAR chip_bx[] = L"Bochs VBE (vcr-kmd test backend)";
    static const WCHAR dac[] = L"3dfx integrated RAMDAC";
    static const WCHAR adapter[] = L"vcr-kmd open 3dfx driver";
    const WCHAR *chip = x->backend == VCR_HW_BOCHS ? chip_bx
                      : VCR_IS_NAPALM(x->device) ? chip_v5 : chip_v3;
    ULONG mem = x->fb_per_chip, n = 0;
    while (chip[n])
        n++;
    VideoPortSetRegistryParameters(x, L"HardwareInformation.ChipType", (PVOID)chip,
                                   (n + 1) * sizeof(WCHAR));
    VideoPortSetRegistryParameters(x, L"HardwareInformation.DacType", (PVOID)dac, sizeof dac);
    VideoPortSetRegistryParameters(x, L"HardwareInformation.MemorySize", &mem, sizeof mem);
    VideoPortSetRegistryParameters(x, L"HardwareInformation.AdapterString", (PVOID)adapter,
                                   sizeof adapter);
}

static VP_STATUS NTAPI VcrFindAdapter(PVOID ext, PVOID ctx, PWSTR args,
                                      PVIDEO_PORT_CONFIG_INFO ci, PUCHAR again)
{
    VCR_EXT *x = (VCR_EXT *)ext;
    VIDEO_ACCESS_RANGE r[6];
    UCHAR cfg[64];
    ULONG attempts, maxatt, slot = 0, i, bar[6];
    VP_STATUS st;
    (void)ctx;
    (void)args;

    *again = FALSE;
    x->cur_mode = -1;
    attempts = VcrDiagGet(L"BootAttempts", 0) + 1;
    maxatt = VcrDiagGet(L"MaxBootAttempts", 3);
    VcrPhase(VCR_EV_FIND_ENTER, attempts, maxatt, "find adapter");

    /* A decline is recorded ON DISK (LastDecline = reason << 16 | attempts):
     * the ring dies with the reboot that follows, and "why is this box on the
     * VGA driver" must be answerable from REGREAD alone. */
    if (VcrDiagGet(L"Disable", 0)) {
        VLOG(VCR_LV_WARN, VCR_EV_SAFE_DECLINE, 1, attempts, 0, 0,
             "Diag\\Disable is set - declining the adapter");
        VcrDiagSet(L"LastDecline", (1u << 16) | (attempts & 0xffff), TRUE);
        return ERROR_DEV_NOT_EXIST;
    }
    if (attempts > maxatt) {
        VLOG(VCR_LV_ERROR, VCR_EV_SAFE_DECLINE, 2, attempts, maxatt, 0,
             "%u boots in a row never reached the stable-boot mark - declining so "
             "XP falls back to VGA", attempts - 1);
        VcrDiagSet(L"DeclinedBoots", VcrDiagGet(L"DeclinedBoots", 0) + 1, FALSE);
        VcrDiagSet(L"LastDecline", (2u << 16) | (attempts & 0xffff), TRUE);
        return ERROR_DEV_NOT_EXIST;
    }
    VcrDiagSet(L"LastDecline", 0, FALSE);
    VcrDiagSet(L"BootAttempts", attempts, TRUE);     /* on disk BEFORE the hardware */
    VcrBootAttempts = attempts;

    VideoPortZeroMemory(r, sizeof r);
    st = VideoPortGetAccessRanges(ext, 0, NULL, 6, r, NULL, NULL, &slot);
    if (st != NO_ERROR) {
        VLOG(VCR_LV_ERROR, VCR_EV_FIND_DONE, st, 0, 0, 0, "GetAccessRanges failed");
        return st;
    }
    x->bus = ci->SystemIoBusNumber;
    x->slot = slot;
    if (VideoPortGetBusData(ext, PCIConfiguration, slot, cfg, 0, sizeof cfg) < 0x30) {
        VLOG(VCR_LV_ERROR, VCR_EV_FIND_DONE, 0, 0, 0, 0, "no PCI config space");
        return ERROR_DEV_NOT_EXIST;
    }
    x->vendor = *(USHORT *)&cfg[0];
    x->device = *(USHORT *)&cfg[2];
    x->revision = cfg[8];
    x->subsys = *(ULONG *)&cfg[0x2c];
    for (i = 0; i < 6; i++)
        bar[i] = *(ULONG *)&cfg[0x10 + 4 * i];
    VLOG(VCR_LV_INFO, VCR_EV_PCI_ID, x->vendor, x->device, x->subsys, x->revision,
         "PCI %04x:%04x subsys %08x rev %u at bus %u slot %x", x->vendor, x->device,
         x->subsys, x->revision, x->bus, slot);

    if (x->vendor == VCR_PCI_VENDOR_3DFX &&
        (x->device == VCR_DEV_BANSHEE || x->device == VCR_DEV_VOODOO3 ||
         VCR_IS_NAPALM(x->device)))
        x->backend = VCR_HW_VOODOO;
    else if (x->vendor == VCR_PCI_VENDOR_BOCHS && x->device == VCR_DEV_BOCHS_VGA)
        x->backend = VCR_HW_BOCHS;
    else {
        VLOG(VCR_LV_ERROR, VCR_EV_FIND_DONE, ERROR_DEV_NOT_EXIST, 0, 0, 0,
             "not a device this driver knows");
        return ERROR_DEV_NOT_EXIST;
    }

    /* Match each assigned range to its BAR by address (the resource list is
     * not guaranteed to be in BAR order). */
    for (i = 0; i < 6 && r[i].RangeLength; i++) {
        ULONG a = r[i].RangeStart.LowPart;
        VLOG(VCR_LV_INFO, VCR_EV_ACCESS_RANGE, i, a, r[i].RangeLength, r[i].RangeInIoSpace,
             "range %u: %08x + %x %s", i, a, r[i].RangeLength,
             r[i].RangeInIoSpace ? "io" : "mem");
        if (r[i].RangeInIoSpace) {
            if (x->backend == VCR_HW_VOODOO && (bar[2] & ~3u) == a) {
                x->chip[0].io_phys = a;
                x->io_len = r[i].RangeLength;
            }
            continue;
        }
        if (x->backend == VCR_HW_VOODOO) {
            if ((bar[0] & ~0xfu) == a) {
                x->chip[0].mmio_phys = r[i].RangeStart;
                x->mmio_len = r[i].RangeLength;
            } else if ((bar[1] & ~0xfu) == a) {
                x->chip[0].lfb_phys = r[i].RangeStart;
                x->lfb_len = r[i].RangeLength;
            }
        } else {
            if ((bar[0] & ~0xfu) == a) {
                x->chip[0].lfb_phys = r[i].RangeStart;
                x->lfb_len = r[i].RangeLength;
            } else if ((bar[2] & ~0xfu) == a) {
                x->chip[0].mmio_phys = r[i].RangeStart;
                x->mmio_len = r[i].RangeLength;
            }
        }
    }
    if (!x->chip[0].mmio_phys.QuadPart || !x->chip[0].lfb_phys.QuadPart) {
        VLOG(VCR_LV_ERROR, VCR_EV_FIND_DONE, ERROR_DEV_NOT_EXIST, bar[0], bar[1], bar[2],
             "a required BAR was not assigned");
        return ERROR_DEV_NOT_EXIST;
    }

    /* kernel mappings: register windows only; the LFB is mapped on request */
    x->chip[0].regs = (PUCHAR)VideoPortGetDeviceBase(ext, x->chip[0].mmio_phys,
                            x->backend == VCR_HW_BOCHS ? 0x1000 : VCR_MMIO_MAP_LEN, FALSE);
    VLOG(x->chip[0].regs ? VCR_LV_INFO : VCR_LV_ERROR, VCR_EV_MAP, 0,
         x->chip[0].mmio_phys.LowPart, VCR_MMIO_MAP_LEN, (ULONG)(ULONG_PTR)x->chip[0].regs,
         "registers mapped");
    if (!x->chip[0].regs)
        return ERROR_NOT_ENOUGH_MEMORY;
    if (x->backend == VCR_HW_BOCHS) {
        x->dispi = (volatile USHORT *)(x->chip[0].regs + 0x500);
        x->bochs_vga = x->chip[0].regs + 0x400;
    } else if (x->chip[0].io_phys) {
        PHYSICAL_ADDRESS io;
        io.QuadPart = x->chip[0].io_phys;
        x->io = (PUCHAR)VideoPortGetDeviceBase(ext, io, 0x100, TRUE);
        VLOG(VCR_LV_INFO, VCR_EV_MAP, 2, x->chip[0].io_phys, 0x100,
             (ULONG)(ULONG_PTR)x->io, "I/O BAR mapped");
    }
    x->nchips = 1;

    VcrPhase(VCR_EV_FIND_DONE, 1, x->backend, "discover");
    st = VcrHwDiscover(x);
    if (st != NO_ERROR) {
        VLOG(VCR_LV_ERROR, VCR_EV_FIND_DONE, st, x->backend, 0, 0, "discovery failed");
        return st;
    }
    VcrHwSaveBootState(x);
    VcrHwSnapshotToLog(x, "boot");
    VcrMultiInit(x);            /* slaves placed + mapped, or Glide stays 1-chip */
    VcrMonitorInit(x);          /* EDID over DDC: the mode list honours the monitor */

    x->nmodes = vcr_modes_build(&x->caps, x->modes, VCR_MAX_MODES);
    /* A full table means modes were DROPPED - it happened silently once
     * (200 of ~210, found by the mode sweep landing on exactly 200). */
    VLOG(x->nmodes >= VCR_MAX_MODES ? VCR_LV_ERROR : VCR_LV_INFO, VCR_EV_MODES_BUILT,
         x->nmodes, vcr_ntimings, x->caps.max_pixclk_khz, x->caps.twox_above_khz,
         x->nmodes >= VCR_MAX_MODES ? "%u modes - TABLE FULL, modes dropped" : "%u modes",
         x->nmodes);
    x->allow_poke = VcrDiagGet(L"AllowPoke", 0);
    x->accel2d = VcrDiagGet(L"Accel2D", 1);
    x->d3d = VcrDiagGet(L"D3D", 1);
    x->texport = VcrDiagGet(L"TexPortFlush", 1);
    hwinfo(x);

    /* no VDM (full-screen DOS) support: the VGA driver keeps that role */
    ci->VdmPhysicalVideoMemoryAddress.QuadPart = 0;
    ci->VdmPhysicalVideoMemoryLength = 0;
    ci->HardwareStateSize = 0;

    VLOG(VCR_LV_INFO, VCR_EV_FIND_DONE, NO_ERROR, x->backend, x->nchips, x->fb_per_chip,
         "adapter ready: %u chip(s), %u MB each", x->nchips, x->fb_per_chip >> 20);
    VcrPhase(VCR_EV_FIND_DONE, 0, x->nchips, "find done");
    return NO_ERROR;
}

static BOOLEAN NTAPI VcrInitialize(PVOID ext)
{
    VCR_EXT *x = (VCR_EXT *)ext;
    VcrPhase(VCR_EV_INIT_ENTER, x->backend, 0, "initialize");
    VideoPortStartTimer(ext);
    VLOG(VCR_LV_INFO, VCR_EV_INIT_DONE, 1, 0, 0, 0, "initialized; stable-boot timer on");
    return TRUE;
}

/* ---- stable-boot mark ------------------------------------------------------------ */

static VOID NTAPI mark_good(PVOID ctx)
{
    VCR_EXT *x = (VCR_EXT *)ctx;
    ULONG was = VcrDiagGet(L"BootAttempts", 0);
    VcrDiagSet(L"BootAttempts", 0, FALSE);
    VcrDiagSet(L"GoodBoots", VcrDiagGet(L"GoodBoots", 0) + 1, TRUE);
    VLOG(VCR_LV_INFO, VCR_EV_SAFE_MARK_OK, was, x->seconds, 0, 0,
         "stable for %u s - boot counter cleared", x->seconds);
}

static VOID NTAPI VcrTimer(PVOID ext)
{
    VCR_EXT *x = (VCR_EXT *)ext;
    x->seconds++;
    if (!x->boot_marked && x->seconds >= 60) {
        x->boot_marked = 1;
        x->work.WorkerRoutine = mark_good;
        x->work.Parameter = x;
        x->work.List.Flink = NULL;
        ExQueueWorkItem(&x->work, VCR_DelayedWorkQueue);
        VLOG(VCR_LV_INFO, VCR_EV_TIMER_STABLE, x->seconds, 0, 0, 0, "stable-boot mark queued");
    }
}

/* ---- reset / power / children ---------------------------------------------------- */

static BOOLEAN NTAPI VcrResetHw(PVOID ext, ULONG cols, ULONG rows)
{
    VCR_EXT *x = (VCR_EXT *)ext;
    VLOG(VCR_LV_WARN, VCR_EV_RESET_HW, cols, rows, 0, 0, "HwResetHw");
    VcrHwResetToVga(x);
    return FALSE;       /* let the HAL finish the text mode */
}

static VP_STATUS NTAPI VcrGetPowerState(PVOID ext, ULONG id, PVIDEO_POWER_MANAGEMENT p)
{
    (void)ext;
    VLOG(VCR_LV_DEBUG, VCR_EV_POWER_GET, id, p->PowerState, 0, 0, "get power");
    if (id == DISPLAY_ADAPTER_HW_ID)
        return p->PowerState == VideoPowerOn || p->PowerState == VideoPowerHibernate
                   ? NO_ERROR : ERROR_DEVICE_REINITIALIZATION_NEEDED;
    return NO_ERROR;
}

static VP_STATUS NTAPI VcrSetPowerState(PVOID ext, ULONG id, PVIDEO_POWER_MANAGEMENT p)
{
    VCR_EXT *x = (VCR_EXT *)ext;
    if (id != DISPLAY_ADAPTER_HW_ID)
        VcrHwPower(x, p->PowerState);
    VLOG(VCR_LV_INFO, VCR_EV_POWER_SET, id, p->PowerState, 0, 0, "set power");
    return NO_ERROR;
}

static VP_STATUS NTAPI VcrGetChildDescriptor(PVOID ext, PVIDEO_CHILD_ENUM_INFO ci,
                                             PVIDEO_CHILD_TYPE type, PUCHAR desc,
                                             PULONG uid, PULONG unused)
{
    (void)unused;
    if (ci->ChildIndex == 1)
        return VcrMonitorChild((VCR_EXT *)ext, ci, type, desc, uid);
    VLOG(VCR_LV_DEBUG, VCR_EV_CHILD, ci->ChildIndex, ERROR_NO_MORE_DEVICES, 0, 0,
         "child enumeration: no child %u", ci->ChildIndex);
    return ERROR_NO_MORE_DEVICES;
}

/* ---- IOCTLs ---------------------------------------------------------------------- */

static void mode_info(VCR_EXT *x, ULONG i, VIDEO_MODE_INFORMATION *m)
{
    const vcr_timing *t = &vcr_timings[x->modes[i].timing];
    ULONG bpp = x->modes[i].bpp;
    VideoPortZeroMemory(m, sizeof *m);
    m->Length = sizeof *m;
    m->ModeIndex = i;
    m->VisScreenWidth = t->w;
    m->VisScreenHeight = t->h;
    m->ScreenStride = t->w * (bpp / 8);
    m->NumberOfPlanes = 1;
    m->BitsPerPlane = bpp;
    m->Frequency = t->refresh;
    m->XMillimeter = 320;
    m->YMillimeter = 240;
    m->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS;
    if (bpp == 8) {
        m->NumberRedBits = m->NumberGreenBits = m->NumberBlueBits = 8;
        m->AttributeFlags |= VIDEO_MODE_PALETTE_DRIVEN | VIDEO_MODE_MANAGED_PALETTE;
    } else if (bpp == 16) {
        m->NumberRedBits = 5;
        m->NumberGreenBits = 6;
        m->NumberBlueBits = 5;
        m->RedMask = 0xf800;
        m->GreenMask = 0x07e0;
        m->BlueMask = 0x001f;
    } else {
        m->NumberRedBits = m->NumberGreenBits = m->NumberBlueBits = 8;
        m->RedMask = 0xff0000;
        m->GreenMask = 0x00ff00;
        m->BlueMask = 0x0000ff;
    }
    m->VideoMemoryBitmapWidth = t->w;
    /* desktop at the top of memory: nothing below it belongs to GDI */
    m->VideoMemoryBitmapHeight = x->desktop_fixed
        ? (x->fb_per_chip - x->desktop_fixed) / m->ScreenStride : t->h;
}

static void fill_info(VCR_EXT *x, vcr_info *v)
{
    ULONG c;
    VideoPortZeroMemory(v, sizeof *v);
    v->size = sizeof *v;
    v->version = VCR_KMD_VERSION_NUM;
    v->build = VCR_KMD_BUILD;
    v->backend = x->backend;
    v->vendor = x->vendor;
    v->device = x->device;
    v->subsys = x->subsys;
    v->revision = x->revision;
    v->bus = x->bus;
    v->slot = x->slot;
    v->nchips = x->nchips;
    for (c = 0; c < x->nchips && c < VCR_MAX_CHIPS; c++) {
        v->chip_slot[c] = c ? x->chip[c].slot : x->slot;
        v->mmio_phys[c] = x->chip[c].mmio_phys.LowPart;
    }
    v->lfb_phys = x->chip[0].lfb_phys.LowPart;
    v->lfb_len = x->lfb_len;
    v->mmio_len = x->mmio_len;
    v->io_base = x->chip[0].io_phys;
    v->io_len = x->io_len;
    v->fb_per_chip = x->fb_per_chip;
    v->desktop_offset = x->desktop_offset;
    v->cur_mode = (ULONG)x->cur_mode;
    if (x->cur_mode >= 0) {
        const vcr_timing *t = &vcr_timings[x->modes[x->cur_mode].timing];
        v->cur_w = t->w;
        v->cur_h = t->h;
        v->cur_bpp = x->modes[x->cur_mode].bpp;
        v->cur_hz = t->refresh;
        v->cur_stride = x->cur_stride;
    }
    v->nmodes = x->nmodes;
    v->boot_attempts = VcrBootAttempts;
    v->boot_good = x->boot_marked;
    v->sli_active = x->sli_active;
    v->glide_chips = x->glide_chips ? x->glide_chips : 1;
    v->sli_chips = x->sli_chips;
    v->sli_result = (vcr_u32)x->sli_result;
    v->clock_6k_hz = x->clock_6k_hz;
    for (c = 0; c < x->nchips && c < VCR_MAX_CHIPS; c++)
        v->slave_bar0[c] = x->chip[c].mmio_phys.LowPart;
    v->edid_ok = x->edid_ok;
    v->mon_filter = x->caps.mon_hmax_khz != 0;
    v->mon_hmin_khz = x->mon.hmin_khz;
    v->mon_hmax_khz = x->mon.hmax_khz;
    v->mon_vmin_hz = x->mon.vmin_hz;
    v->mon_vmax_hz = x->mon.vmax_hz;
    v->mon_max_pixclk_khz = x->mon.max_pixclk_khz;
    VideoPortMoveMemory(v->mon_pnp, x->mon.pnpid, sizeof v->mon_pnp);
    v->mon_product = x->mon.product;
    VideoPortMoveMemory(v->mon_name, x->mon.name, sizeof x->mon.name);
    VideoPortMoveMemory(v->edid, x->edid, sizeof v->edid);
    v->log_next_seq = VcrLogNextSeq();
    v->flags = (x->allow_poke ? VCR_INFO_F_ALLOW_POKE : 0) |
               (x->accel2d ? 0 : VCR_INFO_F_NO_ACCEL2D) | (x->d3d ? 0 : VCR_INFO_F_NO_D3D) |
               (x->texport ? 0 : VCR_INFO_F_NO_TEXPORT);
    v->ogl_version = VcrDiagGet(L"OpenGLVersion", 2);
    v->ogl_driver_version = VcrDiagGet(L"OpenGLDriverVersion", 1);
    if (!VcrDiagGetString(L"OpenGLName", v->ogl_name, 32)) {
        v->ogl_name[0] = '3';
        v->ogl_name[1] = 'd';
        v->ogl_name[2] = 'f';
        v->ogl_name[3] = 'x';
        v->ogl_name[4] = 0;
    }
}

/* A bare if: the `break` must leave the SWITCH (a do/while(0) would swallow it). */
#define NEED_OUT(n)  if (rp->OutputBufferLength < (n)) { st = ERROR_INSUFFICIENT_BUFFER; break; }
#define NEED_IN(n)   if (rp->InputBufferLength < (n)) { st = ERROR_INSUFFICIENT_BUFFER; break; }

static VP_STATUS pci_op(VCR_EXT *x, vcr_pci_op *op)
{
    ULONG slot, size = op->size ? op->size : 4;
    if (size != 1 && size != 2 && size != 4)
        return ERROR_INVALID_PARAMETER;
    if (op->target == VCR_PCI_TARGET_BRIDGE) {
        if (!x->bridge_found)
            return ERROR_DEV_NOT_EXIST;
        if (op->write) {
            if (!x->allow_poke)
                return ERROR_ACCESS_DENIED;
            HalSetBusDataByOffset(VCR_PCIConfiguration, x->bridge_bus, x->bridge_slot,
                                  &op->value, op->offset, size);
        } else {
            op->value = 0;
            HalGetBusDataByOffset(VCR_PCIConfiguration, x->bridge_bus, x->bridge_slot,
                                  &op->value, op->offset, size);
        }
    } else {
        if (op->target >= x->nchips)
            return ERROR_INVALID_PARAMETER;
        slot = op->target ? x->chip[op->target].slot : x->slot;
        if (op->write) {
            /* the standard header is not ours to rewrite casually */
            if (op->offset < 0x40 && !x->allow_poke)
                return ERROR_ACCESS_DENIED;
            VcrPciWrite(x, slot, op->offset, op->value, size);
        } else {
            op->value = VcrPciRead(x, slot, op->offset, size);
        }
    }
    VLOG(op->write ? VCR_LV_INFO : VCR_LV_TRACE, VCR_EV_PCI_OP, op->target, op->offset,
         op->value, op->write, "pci %s", op->write ? "write" : "read");
    return NO_ERROR;
}

static VP_STATUS reg_op(VCR_EXT *x, vcr_reg_op *op)
{
    if (op->chip >= x->nchips)
        return ERROR_INVALID_PARAMETER;
    if (op->write && !x->allow_poke)
        return ERROR_ACCESS_DENIED;
    if (op->kind != VCR_REG_MMIO32 && op->chip != 0)
        return ERROR_INVALID_PARAMETER;
    switch (op->kind) {
    case VCR_REG_MMIO32:
        if (op->offset & 3 || op->offset >= VCR_MMIO_MAP_LEN)
            return ERROR_INVALID_PARAMETER;
        if (op->write)
            VcrWr(x, op->chip, op->offset, op->value);
        else
            op->value = VcrRd(x, op->chip, op->offset);
        break;
    case VCR_REG_VGA_CRTC:
    case VCR_REG_VGA_SEQ:
    case VCR_REG_VGA_GFX: {
        ULONG port = op->kind == VCR_REG_VGA_CRTC ? VCR_VGA_CRTC_I
                   : op->kind == VCR_REG_VGA_SEQ ? VCR_VGA_SEQ_I : VCR_VGA_GFX_I;
        if (op->write)
            VcrVgaIdxWr(x, port, (UCHAR)op->vga_index, (UCHAR)op->value);
        else
            op->value = VcrVgaIdxRd(x, port, (UCHAR)op->vga_index);
        break;
    }
    case VCR_REG_VGA_ATTR:
        if (op->write)
            return ERROR_INVALID_PARAMETER;
        op->value = VcrAttrRd(x, (UCHAR)op->vga_index);
        break;
    case VCR_REG_VGA_PORT:
        if (op->offset < 0x3b0 || op->offset > 0x3df)
            return ERROR_INVALID_PARAMETER;
        if (op->write)
            VcrVgaWr(x, op->offset, (UCHAR)op->value);
        else
            op->value = VcrVgaRd(x, op->offset);
        break;
    default:
        return ERROR_INVALID_PARAMETER;
    }
    if (op->write)
        VLOG(VCR_LV_INFO, VCR_EV_POKE, op->offset, op->value, op->chip, op->kind, "poke");
    return NO_ERROR;
}

static BOOLEAN NTAPI VcrStartIO(PVOID ext, PVIDEO_REQUEST_PACKET rp)
{
    VCR_EXT *x = (VCR_EXT *)ext;
    VP_STATUS st = NO_ERROR;
    ULONG info = 0, code = rp->IoControlCode;

    switch (code) {
    case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES: {
        VIDEO_NUM_MODES *n = (VIDEO_NUM_MODES *)rp->OutputBuffer;
        NEED_OUT(sizeof *n);
        n->NumModes = x->nmodes;
        n->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
        info = sizeof *n;
        break;
    }
    case IOCTL_VIDEO_QUERY_AVAIL_MODES: {
        VIDEO_MODE_INFORMATION *m = (VIDEO_MODE_INFORMATION *)rp->OutputBuffer;
        ULONG i;
        NEED_OUT(x->nmodes * sizeof *m);
        for (i = 0; i < x->nmodes; i++)
            mode_info(x, i, &m[i]);
        info = x->nmodes * sizeof *m;
        break;
    }
    case IOCTL_VIDEO_QUERY_CURRENT_MODE: {
        NEED_OUT(sizeof(VIDEO_MODE_INFORMATION));
        if (x->cur_mode < 0) {
            st = ERROR_INVALID_FUNCTION;
            break;
        }
        mode_info(x, (ULONG)x->cur_mode, (VIDEO_MODE_INFORMATION *)rp->OutputBuffer);
        info = sizeof(VIDEO_MODE_INFORMATION);
        break;
    }
    case IOCTL_VIDEO_SET_CURRENT_MODE: {
        VIDEO_MODE *m = (VIDEO_MODE *)rp->InputBuffer;
        NEED_IN(sizeof *m);
        st = VcrHwSetMode(x, m->RequestedMode & ~VIDEO_MODE_NO_ZERO_MEMORY);
        break;
    }
    case IOCTL_VIDEO_RESET_DEVICE:
        VcrSliOff(x, "display reset");
        VcrHwResetToVga(x);
        break;
    case IOCTL_VIDEO_MAP_VIDEO_MEMORY: {
        VIDEO_MEMORY *in = (VIDEO_MEMORY *)rp->InputBuffer;
        VIDEO_MEMORY_INFORMATION *out = (VIDEO_MEMORY_INFORMATION *)rp->OutputBuffer;
        ULONG inio = VIDEO_MEMORY_SPACE_MEMORY | VIDEO_MEMORY_SPACE_P6CACHE;
        NEED_IN(sizeof *in);
        NEED_OUT(sizeof *out);
        out->VideoRamBase = in->RequestedVirtualAddress;
        out->VideoRamLength = x->fb_per_chip;
        st = VideoPortMapMemory(ext, x->chip[0].lfb_phys, &out->VideoRamLength, &inio,
                                &out->VideoRamBase);
        out->FrameBufferBase = (PUCHAR)out->VideoRamBase + x->desktop_offset;
        out->FrameBufferLength = out->VideoRamLength - x->desktop_offset;
        VLOG(st == NO_ERROR ? VCR_LV_INFO : VCR_LV_ERROR, VCR_EV_MAP, 1,
             x->chip[0].lfb_phys.LowPart, out->VideoRamLength,
             (ULONG)(ULONG_PTR)out->VideoRamBase, "LFB mapped for the display driver");
        info = sizeof *out;
        break;
    }
    case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY: {
        VIDEO_MEMORY *in = (VIDEO_MEMORY *)rp->InputBuffer;
        NEED_IN(sizeof *in);
        st = VideoPortUnmapMemory(ext, in->RequestedVirtualAddress, NULL);
        break;
    }
    case IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES: {
        /* the display driver's own view of chip 0's register window (io,
         * CMDFIFO, 2D, 3D) - what its Direct3D and 2D engines write. System
         * space only: the input names a process to map into (NULL = the
         * display driver), and no other process is given the registers here -
         * Glide has IOCTL_VCR_MAP_GLIDE. */
        VIDEO_MEMORY req;
        VIDEO_PUBLIC_ACCESS_RANGES *out = (VIDEO_PUBLIC_ACCESS_RANGES *)rp->OutputBuffer;
        ULONG len = VCR_DD_MAP_LEN, inio = VIDEO_MEMORY_SPACE_MEMORY;
        PVOID va = NULL;
        NEED_OUT(sizeof *out);
        req.RequestedVirtualAddress = NULL;
        if (rp->InputBufferLength >= sizeof req)        /* one buffer: read first */
            VideoPortMoveMemory(&req, rp->InputBuffer, sizeof req);
        if (x->backend != VCR_HW_VOODOO || req.RequestedVirtualAddress) {
            st = ERROR_INVALID_FUNCTION;
            break;
        }
        st = VideoPortMapMemory(ext, x->chip[0].mmio_phys, &len, &inio, &va);
        out->InIoSpace = 0;
        out->MappedInIoSpace = inio;
        out->VirtualAddress = st == NO_ERROR ? va : NULL;
        info = sizeof *out;
        VLOG(st == NO_ERROR ? VCR_LV_INFO : VCR_LV_ERROR, VCR_EV_MAP, 2,
             x->chip[0].mmio_phys.LowPart, len, (ULONG)(ULONG_PTR)va,
             "registers mapped for the display driver");
        break;
    }
    case IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES: {
        VIDEO_MEMORY req;
        NEED_IN(sizeof req);
        VideoPortMoveMemory(&req, rp->InputBuffer, sizeof req);
        st = VideoPortUnmapMemory(ext, req.RequestedVirtualAddress, NULL);
        VLOG(st == NO_ERROR ? VCR_LV_INFO : VCR_LV_WARN, VCR_EV_MAP, 3,
             (ULONG)(ULONG_PTR)req.RequestedVirtualAddress, st, 0, "registers unmapped");
        break;
    }
    case IOCTL_VIDEO_GET_CHILD_STATE: {
        /* the monitor child (vcrmp_ddc.c) is there and powered: XP polls this
         * once a child is reported, and every refusal was a WARN in the log */
        ULONG uid;
        NEED_IN(sizeof(ULONG));
        NEED_OUT(sizeof(ULONG));
        uid = *(ULONG *)rp->InputBuffer;            /* one buffer: read first */
        *(ULONG *)rp->OutputBuffer = VIDEO_CHILD_ACTIVE;
        info = sizeof(ULONG);
        VLOG(VCR_LV_TRACE, VCR_EV_CHILD, uid, VIDEO_CHILD_ACTIVE, 0, 0, "child %x state: active", uid);
        break;
    }
    case IOCTL_VIDEO_SHARE_VIDEO_MEMORY:
    case IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY:
    case IOCTL_VCR_DDFLIP:
    case IOCTL_VCR_VBLANK:
        st = VcrDdIoctl(x, code, rp->InputBuffer, rp->InputBufferLength, rp->OutputBuffer,
                        rp->OutputBufferLength, &info);
        break;
    case IOCTL_VIDEO_QUERY_POINTER_CAPABILITIES:
    case IOCTL_VIDEO_SET_POINTER_ATTR:
    case IOCTL_VIDEO_SET_POINTER_POSITION:
    case IOCTL_VIDEO_ENABLE_POINTER:
    case IOCTL_VIDEO_DISABLE_POINTER:
        st = VcrCursorIoctl(x, code, rp->InputBuffer, rp->InputBufferLength,
                            rp->OutputBuffer, rp->OutputBufferLength, &info);
        break;
    case IOCTL_VIDEO_SET_COLOR_REGISTERS:
        st = VcrHwSetClut(x, (VIDEO_CLUT *)rp->InputBuffer, rp->InputBufferLength);
        break;

    /* ---- private -------------------------------------------------------------- */
    case IOCTL_VCR_INFO:
        NEED_OUT(sizeof(vcr_info));
        fill_info(x, (vcr_info *)rp->OutputBuffer);
        info = sizeof(vcr_info);
        break;
    case IOCTL_VCR_LOG_WRITE:
        NEED_IN(sizeof(vcr_log_write_req));
        VcrLogFromUser((const vcr_log_write_req *)rp->InputBuffer);
        break;
    case IOCTL_VCR_LOG_READ: {
        vcr_log_read_req req;
        vcr_log_read_res *out = (vcr_log_read_res *)rp->OutputBuffer;
        ULONG room;
        NEED_IN(sizeof req);
        NEED_OUT(VCR_LOG_READ_RES_BYTES(1));
        req = *(vcr_log_read_req *)rp->InputBuffer;     /* in and out share a buffer */
        room = (rp->OutputBufferLength - VCR_LOG_READ_RES_BYTES(0)) / sizeof(vcr_log_entry);
        if (req.max && req.max < room)
            room = req.max;
        VcrLogRead(req.after_seq, out, room);
        info = VCR_LOG_READ_RES_BYTES(out->count);
        break;
    }
    case IOCTL_VCR_MAP_GLIDE:
        NEED_OUT(sizeof(vcr_glide_map));
        st = VcrMapGlide(x, (vcr_glide_map *)rp->OutputBuffer);
        info = sizeof(vcr_glide_map);
        break;
    case IOCTL_VCR_UNMAP_GLIDE:
        st = VcrUnmapGlide(x, 0);
        break;
    case IOCTL_VCR_PCI_OP: {
        vcr_pci_op op;
        NEED_IN(sizeof op);
        NEED_OUT(sizeof op);
        op = *(vcr_pci_op *)rp->InputBuffer;
        st = pci_op(x, &op);
        *(vcr_pci_op *)rp->OutputBuffer = op;
        info = sizeof op;
        break;
    }
    case IOCTL_VCR_REG: {
        vcr_reg_op op;
        NEED_IN(sizeof op);
        NEED_OUT(sizeof op);
        op = *(vcr_reg_op *)rp->InputBuffer;
        st = reg_op(x, &op);
        *(vcr_reg_op *)rp->OutputBuffer = op;
        info = sizeof op;
        break;
    }
    case IOCTL_VCR_SNAPSHOT:
        NEED_OUT(sizeof(vcr_snapshot));
        VcrHwSnapshot(x, (vcr_snapshot *)rp->OutputBuffer);
        info = sizeof(vcr_snapshot);
        break;
    case IOCTL_VCR_BOOT_OK:
        VcrDiagSet(L"BootAttempts", 0, TRUE);
        VLOG(VCR_LV_INFO, VCR_EV_SAFE_MARK_OK, VcrBootAttempts, 0, 1, 0,
             "boot counter cleared on request");
        break;
    case IOCTL_VCR_RESTORE_MODE:
        st = VcrHwRestoreMode(x);
        break;
    case IOCTL_VCR_SLI:
        NEED_OUT(sizeof(vcr_sli_res));
        st = VcrSliRequest(x, rp->InputBuffer, rp->InputBufferLength,
                           (vcr_sli_res *)rp->OutputBuffer);
        info = sizeof(vcr_sli_res);
        break;
    case IOCTL_VCR_RESET_ENGINE:
        st = VcrHwResetEngine(x, 0, "requested") ? NO_ERROR : ERROR_BUSY;
        break;
    default:
        VLOG(VCR_LV_DEBUG, VCR_EV_IOCTL_UNKNOWN, code, rp->InputBufferLength,
             rp->OutputBufferLength, 0, "unsupported IOCTL %x", code);
        st = ERROR_INVALID_FUNCTION;
        break;
    }
    if (code != IOCTL_VCR_LOG_READ && code != IOCTL_VCR_LOG_WRITE)
        VLOG(st == NO_ERROR ? VCR_LV_TRACE : VCR_LV_WARN, VCR_EV_IOCTL, code,
             rp->InputBufferLength, rp->OutputBufferLength, st, "ioctl %x -> %u", code, st);
    rp->StatusBlock->Status = st;
    rp->StatusBlock->Information = info;
    return TRUE;
}
