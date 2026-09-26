#!/usr/bin/env python3
"""sli_compare.py - diff two live multi-chip captures (sli_golden.py output).

    sli_compare.py golden/sli_amigamerlin-3.1-r11_cfg5_192.168.1.124.json \
                   golden/sli_vcrkmd-sli_cfg5_192.168.1.124.json

Compares, per chip, the PCI config dwords that carry SLI/AA state
(0x40-0xAC: cfgInitEnable, cfgPciDecode, cfgVideoCtrl0-2, cfgSliLfbCtrl,
cfgAADepthBufferAperture, cfgAALfbCtrl, cfgSliAAMisc, ...) plus the BARs and
command register; the HiNT bridge's GPIO (0xC4, the V5 6000 clock); every
chip's IO registers (memBase0 0x00-0xFC); and the 3D sliCtrl/aaCtrl pair.
Registers that move by themselves (status, the vertical counter, the command
FIFO pointers) are listed as volatile and never reported.

Exit 0 when nothing but volatile registers differ.
"""
import json
import struct
import sys

# memBase0 IO register names (h3regs.h order), for readable output
IOREG = {
    0x00: "status", 0x04: "pciInit0", 0x08: "sipMonitor", 0x0c: "lfbMemoryConfig",
    0x10: "miscInit0", 0x14: "miscInit1", 0x18: "dramInit0", 0x1c: "dramInit1",
    0x20: "agpInit0", 0x24: "tmuGbeInit", 0x28: "vgaInit0", 0x2c: "vgaInit1",
    0x30: "dramCommand", 0x34: "dramData", 0x40: "pllCtrl0", 0x44: "pllCtrl1",
    0x48: "pllCtrl2", 0x4c: "dacMode", 0x50: "dacAddr", 0x54: "dacData",
    0x58: "rgbMaxDelta", 0x5c: "vidProcCfg", 0x60: "hwCurPatAddr", 0x64: "hwCurLoc",
    0x68: "hwCurC0", 0x6c: "hwCurC1", 0x70: "vidInFormat", 0x74: "vidTvOutBlankVCount",
    0x78: "vidSerialParallelPort", 0x7c: "vidInXDecimDeltas", 0x80: "vidInDecimInitErrs",
    0x84: "vidInYDecimDeltas", 0x88: "vidPixelBufThold", 0x8c: "vidChromaMin",
    0x90: "vidChromaMax", 0x94: "vidCurrentLine", 0x98: "vidScreenSize",
    0x9c: "vidOverlayStartCoords", 0xa0: "vidOverlayEndScreenCoord",
    0xa4: "vidOverlayDudx", 0xa8: "vidOverlayDudxOffsetSrcWidth", 0xac: "vidOverlayDvdy",
    0xe4: "vidDesktopStartAddr", 0xe8: "vidDesktopOverlayStride",
    0xec: "vidInAddr0", 0xf0: "vidInAddr1", 0xf4: "vidInAddr2", 0xf8: "vidInStride",
    0xfc: "vidCurrOverlayStartAddr",
}
CFG = {
    0x04: "command", 0x10: "memBase0", 0x14: "memBase1", 0x18: "ioBase",
    0x40: "cfgInitEnable", 0x44: "acpiReset", 0x48: "cfgPciDecode",
    0x4c: "cfgStatus", 0x50: "cfgScratch", 0x54: "agpCapabilities", 0x58: "agpStatus",
    0x5c: "agpCommand", 0x60: "acpiCapabilities", 0x64: "acpiControlStatus",
    0x80: "cfgVideoCtrl0", 0x84: "cfgVideoCtrl1", 0x88: "cfgVideoCtrl2",
    0x8c: "cfgSliLfbCtrl", 0x90: "cfgAADepthBufferAperture", 0x94: "cfgAALfbCtrl",
    0xac: "cfgSliAAMisc",
}
CFG_OFFS = [0x04, 0x10, 0x14, 0x18] + list(range(0x40, 0xb0, 4))
# registers that move while a frame runs, or hold leftovers of no meaning:
# status, dacAddr/dacData, vidTvOutBlankVCount, vidCurrentLine, dramData (the
# last SGRAM command's operand), vidSerialParallelPort (DDC/I2C line levels),
# vidCurrOverlayStartAddr (whichever buffer is on screen this frame)
VOLATILE_IO = {0x00, 0x34, 0x50, 0x54, 0x74, 0x78, 0x94, 0xfc}
# command (its status half moves) and cfgStatus (the live status register's alias)
VOLATILE_CFG = {0x04, 0x4c}
# memBase0 0xB0-0xDF is the VGA alias, which does NOT read back through MMIO on a
# VSA-100 (it returns status-like junk) - not a register comparison at all
VGA_ALIAS_IO = set(range(0xb0, 0xe0, 4))


def dw(hexs, off):
    b = bytes.fromhex(hexs)
    return struct.unpack_from("<I", b, off)[0] if off + 4 <= len(b) else None


def cmp(a, b):
    sa, sb = a["samples"][-1], b["samples"][-1]
    diffs = 0
    for key in sorted(set(sa["pci"]) | set(sb["pci"])):
        ca, cb = sa["pci"].get(key), sb["pci"].get(key)
        if not ca or not cb:
            print(f"{key}: missing in {'A' if not ca else 'B'}")
            diffs += 1
            continue
        offs = [0xc4] if key.startswith("bridge") else CFG_OFFS
        for o in offs:
            va, vb = dw(ca, o), dw(cb, o)
            if va == vb:
                continue
            if o == 0x04:           # compare the command half only
                if (va & 0xffff) == (vb & 0xffff):
                    continue
            elif o in VOLATILE_CFG and not key.startswith("bridge"):
                continue
            name = "gpio" if key.startswith("bridge") else CFG.get(o, f"cfg{o:02x}")
            print(f"{key:14s} cfg {o:02x} {name:26s} A {va:08x}  B {vb:08x}")
            diffs += 1
    for chip in sorted(set(sa["mem"]) | set(sb["mem"])):
        ma, mb = sa["mem"].get(chip), sb["mem"].get(chip)
        if not ma or not mb or not ma.get("io") or not mb.get("io"):
            print(f"{chip}: IO registers missing in {'A' if not (ma and ma.get('io')) else 'B'}")
            diffs += 1
            continue
        for i, (va, vb) in enumerate(zip(ma["io"], mb["io"])):
            o = i * 4
            if va == vb or o in VOLATILE_IO or o in VGA_ALIAS_IO:
                continue
            print(f"{chip:14s} io  {o:02x} {IOREG.get(o, f'io{o:02x}'):26s} A {va}  B {vb}")
            diffs += 1
        for i, (va, vb) in enumerate(zip(ma.get("3d_200") or [], mb.get("3d_200") or [])):
            if va != vb:
                print(f"{chip:14s} 3d  {0x200 + i * 4:03x} {'':26s} A {va}  B {vb}")
                diffs += 1
    return diffs


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = (json.load(open(p)) for p in sys.argv[1:])
    print(f"A = {a.get('label')} cfg {a.get('cfg')} ({a.get('taken')})")
    print(f"B = {b.get('label')} cfg {b.get('cfg')} ({b.get('taken')})")
    n = cmp(a, b)
    print(f"{n} difference(s)")
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
