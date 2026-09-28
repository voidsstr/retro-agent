#!/usr/bin/env python3
"""newdark_dis.py - reproduce the static evidence for "why NewDark's Direct3D 9
display refuses vcr-kmd" from the staged Thief2.exe (NewDark 1.26).

    python3 newdark_dis.py [path/to/Thief2.exe] > excerpts.asm

Needs pefile + capstone (both on the dev host). Prints each named excerpt with
the strings its immediates point at, so the listing reads without a debugger.
The addresses are for the build below; a different Thief2.exe is refused
rather than silently disassembled at the wrong places.

D3D9 interface offsets used in the excerpts (vtable byte offsets):
  IDirect3D9        +0x18 GetAdapterModeCount  +0x1c EnumAdapterModes
                    +0x24 CheckDeviceType      +0x28 CheckDeviceFormat
                    +0x2c CheckDeviceMultiSampleType
                    +0x30 CheckDepthStencilMatch  +0x38 GetDeviceCaps
                    +0x40 CreateDevice
  IDirect3DDevice9  +0x40 Reset  +0x48 GetBackBuffer  +0x5c CreateTexture
                    +0x70 CreateRenderTarget  +0x118 ValidateDevice
D3DCAPS9 offsets: +0x1c DevCaps +0x24 RasterCaps +0x38 ShadeCaps
                  +0x3c TextureCaps +0x94 MaxTextureBlendStages
                  +0x98 MaxSimultaneousTextures +0x8c FVFCaps
"""
import hashlib
import os
import re
import struct
import sys

import capstone
import pefile

DEFAULT = "/mnt/retro-share/Files/Games-Library/Thief2/Thief2.exe"
MD5 = "1109955d4d0a7855592b33d954abcb76"

EXCERPTS = [
    ("display_select: use_d3d_display -> D3D9 enumeration (0x601d60) or DX6",
     0x5e93b0, 0x5e9538),
    ("dx6_device_validation: what the legacy DX6 display asks of a HAL device",
     0x5e8be2, 0x5e8e53),
    ("scrn_loop: requested mode, fallback mode, then the dialog",
     0x5c87c9, 0x5c8970),
    ("d3d9_device_validation (D3DCAPS9 in esi)", 0x601150, 0x6012e4),
    ("d3d9_enum_adapter: GetDeviceCaps, modes of R5G6B5 + X8R8G8B8, validation",
     0x6014c0, 0x601855),
    ("provider caps: TextureCaps POW2 / NONPOW2CONDITIONAL -> flag 8",
     0x681681, 0x681734),
    ("StartMode: d3d_disp_2d_surf_mode (default 3, forced to 0 without flag 8)",
     0x681d25, 0x681dac),
    ("StartMode: back buffer format, depth, multisample, CreateDevice HW then SW VP",
     0x68218e, 0x6825f7),
    ("StartMode: the 2D layer - pow2(screen) 32-bit texture, failure returns 0",
     0x682853, 0x682b30),
    ("hw 2D surface (modes 1-4): CreateTexture DYNAMIC / RENDERTARGET, DEFAULT pool",
     0x6831f0, 0x6832e1),
    ("multisample: steps multisampletype down to NONE", 0x682f10, 0x682fb6),
    ("depth format: D32 D24S8 D24X4S4 D24X8 D16 D15S1, format + match",
     0x682fc0, 0x683073),
]


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    data = open(exe, "rb").read()
    md5 = hashlib.md5(data).hexdigest()
    if md5 != MD5:
        sys.exit("%s: md5 %s, the excerpts are for %s - re-derive the addresses"
                 % (exe, md5, MD5))
    pe = pefile.PE(data=data, fast_load=True)
    ib = pe.OPTIONAL_HEADER.ImageBase
    secs = [(s.VirtualAddress + ib, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]

    def va2off(v):
        for va, raw, size in secs:
            if va <= v < va + size:
                return raw + v - va
        return None

    def cstr(v):
        o = va2off(v)
        if o is None:
            return None
        s = data[o:data.find(b"\0", o)]
        if len(s) >= 3 and all(32 <= c < 127 for c in s):
            return s.decode()
        return None

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    print("; %s  md5 %s" % (os.path.basename(exe), md5))
    for title, a, b in EXCERPTS:
        print("\n; ==== %s  [%08x-%08x]" % (title, a, b))
        o = va2off(a)
        for ins in md.disasm(data[o:o + (b - a)], a):
            if ins.mnemonic == "int3":
                continue
            note = ""
            for m in re.findall(r"0x[0-9a-f]{6,8}", ins.op_str):
                s = cstr(int(m, 16))
                if s:
                    note += '  ; "%s"' % s[:90]
            print("%08x: %-8s %s%s" % (ins.address, ins.mnemonic, ins.op_str, note))
    return 0


if __name__ == "__main__":
    sys.exit(main())
