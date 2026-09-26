/*
 * vcrdd_escape.c - DrvEscape: the HWCEXT protocol our Glide speaks, XP's
 * OPENGL_GETINFO, and the vcr-kmd harness escapes.
 *
 * HWCEXT contract (docs/survey-glide-kernel-contract.md): success is an
 * ExtEscape return > 0 AND resStatus == 1. Fatal to Glide if refused:
 * GETDEVICECONFIG, GETLINEARADDR, HWCSETEXCLUSIVE (and GET_SLAVE_REGS when
 * numChips > 1). Every request is logged with its answer - Glide's own
 * failure messages name the step, the log names the reason.
 *
 * Multi-chip: GETDEVICECONFIG reports the chips the miniport has placed and
 * mapped (vcr_info.glide_chips - 1 when Diag\\Sli=0 or placement failed), and
 * SLI_AA_REQUEST goes to the miniport (IOCTL_VCR_SLI), which runs the Glide GPL
 * dos_mode.c sequence (vcrmp_sli.c) and programs the V5 6000's clock.
 */
#include "vcrdd.h"

#ifndef QUERYESCSUPPORT
#define QUERYESCSUPPORT 8
#endif
#ifndef OPENGL_GETINFO
#define OPENGL_GETINFO 4353
#endif

typedef struct {
    ULONG ulVersion;
    ULONG ulDriverVersion;
    WCHAR awch[261];
} VCR_OPENGL_INFO;

static BOOL supported(ULONG esc)
{
    switch (esc) {
    case QUERYESCSUPPORT:
    case OPENGL_GETINFO:
    case VCR_EXT_HWC:
    case VCR_EXT_HWC_OLD:
    case VCR_EXT_HWC_WXP:
        return TRUE;
    }
    return esc > VCR_ESC_BASE && esc <= VCR_ESC_RESET_ENGINE;
}

static BOOL get_info(VCR_PDEV *pd, vcr_info *v)
{
    return VcrIoctl(pd->hDriver, IOCTL_VCR_INFO, NULL, 0, v, sizeof *v, NULL) == 0;
}

/* ---- HWCEXT ------------------------------------------------------------------ */

static void hwc(VCR_PDEV *pd, const vcr_hwc_req *rq, vcr_hwc_res *rs, ULONG cjOut)
{
    vcr_info info;
    vcr_glide_map map;
    ULONG pid = (ULONG)(ULONG_PTR)EngGetCurrentProcessId(), i;
    DWORD rc;

    memset(rs, 0, cjOut < sizeof *rs ? cjOut : sizeof *rs);
    rs->resStatus = VCR_HWC_FAIL;
    pd->hwc_requests++;

    switch (rq->which) {
    case VCR_HWC_GETDRIVERVERSION:
        rs->opt.driverVersion.major = VCR_KMD_VERSION_MAJOR;
        rs->opt.driverVersion.minor = VCR_KMD_VERSION_MINOR;
        rs->resStatus = VCR_HWC_OK;
        break;

    case VCR_HWC_GETDEVICECONFIG:
        if (!get_info(pd, &info))
            break;
        rs->opt.deviceConfig.devNum = rq->opt.deviceConfig.devNo;
        rs->opt.deviceConfig.vendorID = info.vendor;
        rs->opt.deviceConfig.deviceID = info.device;
        rs->opt.deviceConfig.fbRam = info.fb_per_chip;
        rs->opt.deviceConfig.chipRev = info.revision;
        rs->opt.deviceConfig.pciStride = 0;     /* windowed Glide only */
        rs->opt.deviceConfig.hwStride = 0;
        rs->opt.deviceConfig.tileMark = 0;
        rs->opt.deviceConfig.isMaster = 1;
        rs->opt.deviceConfig.numChips = info.glide_chips ? info.glide_chips : 1;
        rs->resStatus = VCR_HWC_OK;
        VcrDd(VCR_LV_INFO, VCR_EV_HWC_DEVCONFIG, info.device, info.fb_per_chip,
              rs->opt.deviceConfig.numChips, 0, "GETDEVICECONFIG vendor %x", info.vendor);
        break;

    case VCR_HWC_GETLINEARADDR:
        rc = VcrIoctl(pd->hDriver, IOCTL_VCR_MAP_GLIDE, NULL, 0, &map, sizeof map, NULL);
        if (rc || map.status)
            break;
        rs->opt.linearAddr.numBaseAddrs = 2;
        rs->opt.linearAddr.baseAddresses[0] = map.base0;
        rs->opt.linearAddr.baseAddresses[1] = map.base1;
        rs->resStatus = VCR_HWC_OK;
        VcrDd(VCR_LV_INFO, VCR_EV_HWC_LINADDR, map.base0, map.base1, map.base1_len, pid,
              "GETLINEARADDR");
        break;

    case VCR_HWC_GET_SLAVE_REGS:
        i = rq->opt.slaveReg.DeviceId;
        rc = VcrIoctl(pd->hDriver, IOCTL_VCR_MAP_GLIDE, NULL, 0, &map, sizeof map, NULL);
        if (rc || i == 0 || i >= map.nchips || i >= VCR_MAX_CHIPS)
            break;
        for (rc = 0; rc < 4; rc++)
            rs->opt.slaveReg.Regs[rc] = map.slave[i][rc];
        rs->resStatus = rs->opt.slaveReg.Regs[0] ? VCR_HWC_OK : VCR_HWC_FAIL;
        VcrDd(VCR_LV_INFO, VCR_EV_HWC_SLAVE, i, map.slave[i][0], map.slave[i][1],
              map.slave[i][3], "GET_SLAVE_REGS");
        break;

    case VCR_HWC_HWCSETEXCLUSIVE:
        pd->exclusive_pid = pid;
        rs->resStatus = VCR_HWC_OK;
        VcrDd(VCR_LV_INFO, VCR_EV_HWC_EXCLUSIVE, 1, pid, 1, 0, "HWCSETEXCLUSIVE");
        break;

    case VCR_HWC_HWCRLSEXCLUSIVE:
        /* Glide reprogrammed the video processor, the LFB tiling and the Y
         * origin for itself: put the desktop mode back before GDI draws. */
        rc = VcrIoctl(pd->hDriver, IOCTL_VCR_RESTORE_MODE, NULL, 0, NULL, 0, NULL);
        pd->exclusive_pid = 0;
        rs->resStatus = rc ? VCR_HWC_FAIL : VCR_HWC_OK;
        VcrDd(VCR_LV_INFO, VCR_EV_HWC_EXCLUSIVE, 0, pid, rs->resStatus, rc,
              "HWCRLSEXCLUSIVE");
        break;

    case VCR_HWC_UNMAP_MEMORY:
        VcrIoctl(pd->hDriver, IOCTL_VCR_UNMAP_GLIDE, NULL, 0, NULL, 0, NULL);
        rs->resStatus = VCR_HWC_OK;
        break;

    case VCR_HWC_PCI_OP: {
        vcr_pci_op op;
        op.target = rq->opt.pciOp.DeviceId & 3;
        op.write = rq->opt.pciOp.Operation == VCR_HWC_PCI_WRITE;
        op.offset = rq->opt.pciOp.Offset;
        op.value = rq->opt.pciOp.Value;
        op.size = 4;
        rc = VcrIoctl(pd->hDriver, IOCTL_VCR_PCI_OP, &op, sizeof op, &op, sizeof op, NULL);
        rs->opt.pciOp.Value = op.value;
        rs->resStatus = rc ? VCR_HWC_FAIL : VCR_HWC_OK;
        break;
    }

    case VCR_HWC_SLI_AA_REQUEST: {
        const vcr_sli_chipinfo *ci = &rq->opt.sliAA.ChipInfo;
        vcr_sli_res sr;
        memset(&sr, 0, sizeof sr);
        rc = VcrIoctl(pd->hDriver, IOCTL_VCR_SLI, (PVOID)&rq->opt.sliAA,
                      sizeof rq->opt.sliAA, &sr, sizeof sr, NULL);
        /* < 0 refused (nothing written); > 0 done with warnings - still done */
        rs->resStatus = (!rc && (LONG)sr.result >= 0) ? VCR_HWC_OK : VCR_HWC_FAIL;
        VcrDd(rs->resStatus == VCR_HWC_OK && !sr.result ? VCR_LV_INFO : VCR_LV_WARN,
              VCR_EV_HWC_SLIAA, ci->dwChips, ci->dwsliEn | (ci->dwaaEn << 1), sr.result,
              sr.clock_6k_hz, "SLI_AA_REQUEST -> ioctl %u, result %d, %u chips live", rc,
              (LONG)sr.result, sr.sli_chips);
        break;
    }

    case VCR_HWC_VIDTIMING:             /* Glide ignores the answer */
        rs->resStatus = VCR_HWC_OK;
        break;

    default:                            /* windowed-surface ops, AGP, ctx dword */
        break;
    }
    VcrDd(rs->resStatus == VCR_HWC_OK ? VCR_LV_DEBUG : VCR_LV_WARN, VCR_EV_HWC_REQUEST,
          rq->which, pid, (ULONG)rs->resStatus, 1, "hwcext %x", rq->which);
}

/* ---- DrvEscape --------------------------------------------------------------- */

ULONG APIENTRY DrvEscape(SURFOBJ *pso, ULONG iEsc, ULONG cjIn, PVOID pvIn,
                         ULONG cjOut, PVOID pvOut)
{
    VCR_PDEV *pd = pso ? (VCR_PDEV *)pso->dhpdev : NULL;
    DWORD rc, got = 0;

    if (!pd)
        return 0;

    switch (iEsc) {
    case QUERYESCSUPPORT:
        return (cjIn >= sizeof(ULONG) && pvIn && supported(*(ULONG *)pvIn)) ? 1 : 0;

    case OPENGL_GETINFO: {
        vcr_info info;
        VCR_OPENGL_INFO *oi = (VCR_OPENGL_INFO *)pvOut;
        ULONG i;
        if (!oi || cjOut < sizeof *oi || !get_info(pd, &info))
            return 0;
        memset(oi, 0, sizeof *oi);
        oi->ulVersion = info.ogl_version;
        oi->ulDriverVersion = info.ogl_driver_version;
        for (i = 0; i < 31 && info.ogl_name[i]; i++)
            oi->awch[i] = info.ogl_name[i];
        VcrDd(VCR_LV_INFO, VCR_EV_DD_ESCAPE, iEsc, cjIn, cjOut, 1, "OPENGL_GETINFO");
        return 1;
    }

    case VCR_EXT_HWC:
    case VCR_EXT_HWC_OLD:
    case VCR_EXT_HWC_WXP:
        if (!pvIn || cjIn < 8 || !pvOut || cjOut < sizeof(vcr_i32))
            return 0;
        hwc(pd, (const vcr_hwc_req *)pvIn, (vcr_hwc_res *)pvOut, cjOut);
        return 1;

    case VCR_ESC_INFO:
        rc = VcrIoctl(pd->hDriver, IOCTL_VCR_INFO, NULL, 0, pvOut, cjOut, &got);
        return rc ? 0 : got;

    case VCR_ESC_LOG_READ:
        rc = VcrIoctl(pd->hDriver, IOCTL_VCR_LOG_READ, pvIn, cjIn, pvOut, cjOut, &got);
        return rc ? 0 : got;

    case VCR_ESC_LOG_MARK:
        if (!pvIn || cjIn < sizeof(vcr_log_write_req))
            return 0;
        ((vcr_log_write_req *)pvIn)->src = VCR_SRC_TOOL;
        return VcrIoctl(pd->hDriver, IOCTL_VCR_LOG_WRITE, pvIn, cjIn, NULL, 0, NULL) ? 0 : 1;

    case VCR_ESC_REG:
        rc = VcrIoctl(pd->hDriver, IOCTL_VCR_REG, pvIn, cjIn, pvOut, cjOut, &got);
        return rc ? 0 : got;

    case VCR_ESC_PCI:
        rc = VcrIoctl(pd->hDriver, IOCTL_VCR_PCI_OP, pvIn, cjIn, pvOut, cjOut, &got);
        return rc ? 0 : got;

    case VCR_ESC_SNAPSHOT:
        rc = VcrIoctl(pd->hDriver, IOCTL_VCR_SNAPSHOT, NULL, 0, pvOut, cjOut, &got);
        return rc ? 0 : got;

    case VCR_ESC_RESET_ENGINE:
        return VcrIoctl(pd->hDriver, IOCTL_VCR_RESET_ENGINE, NULL, 0, NULL, 0, NULL) ? 0 : 1;

    case VCR_ESC_BOOT_OK:
        return VcrIoctl(pd->hDriver, IOCTL_VCR_BOOT_OK, NULL, 0, NULL, 0, NULL) ? 0 : 1;

    case VCR_ESC_DD_STATS:
        if (!pvOut || cjOut < 4 * sizeof(ULONG))
            return 0;
        ((ULONG *)pvOut)[0] = pd->ulMode;
        ((ULONG *)pvOut)[1] = pd->exclusive_pid;
        ((ULONG *)pvOut)[2] = pd->hwc_requests;
        ((ULONG *)pvOut)[3] = (ULONG)(ULONG_PTR)pd->pjScreen;
        return 4 * sizeof(ULONG);
    }
    return 0;
}
