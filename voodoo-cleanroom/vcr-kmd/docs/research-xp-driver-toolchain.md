# Building, loading and debugging an XP display driver pair from Linux

Research notes, 2026-09-25. What is verified here vs. read elsewhere is marked.

## Toolchain: mingw-w64 i686 (gcc 13, binutils 2.45.90) — no DDK needed

- `winddi.h` includes `<ddrawint.h>` and `<d3dnthal.h>`, which the mingw package
  does not ship: we carry opaque stubs in `compat/` until DirectDraw is built.
- **mingw's `libntoskrnl.a` exports ~705 names XP SP3 lacks, `memcmp` among
  them.** The link succeeds and the driver then fails to load. XP's
  `win32k.sys` exports no `mem*`/`str*` at all. So: bring our own
  `memcpy/memset/memmove/memcmp`, compiled with
  `-fno-tree-loop-distribute-patterns` (else gcc turns the loop back into a
  call to itself), and **check every import of the built binaries against the
  real XP SP3 export tables** (`tools/check_imports.py`, reference binaries
  from an XP SP3 `I386` tree: videoprt.sys, ntoskrnl.exe, hal.dll, win32k.sys).
  Verified locally: all 112 names in `libvideoprt.a` and 197 in `libwin32k.a`
  exist in XP SP3 5.1.2600.5512.
- Flags (both binaries): `-O2 -march=i686 -ffreestanding -mgeneral-regs-only
  -mno-stack-arg-probe -fno-stack-protector -fno-asynchronous-unwind-tables
  -fno-common -fno-strict-aliasing -D_X86_ -Di386 -DSTD_CALL
  -D_WIN32_WINNT=0x0501`. Link: `-nostdlib -nostartfiles -shared
  -Wl,--subsystem,native:5.01 -Wl,--image-base,0x10000
  -Wl,--exclude-all-symbols -Wl,--disable-auto-import
  -Wl,--disable-stdcall-fixup`; miniport `--entry,_DriverEntry@8 -lvideoprt
  -lgcc`; display DLL `--entry,_DrvEnableDriver@12` + a .def + `-lwin32k -lgcc`.
- `-mno-stack-arg-probe`: without it a large frame calls `___chkstk_ms`. The XP
  x86 kernel stack is 12 KB — keep frames small anyway.
- **Every `Hw*` callback must be `NTAPI`** (mingw only warns; the result is a
  corrupted stack).
- FPU: forbidden without `EngSaveFloatingPointState`; `-mgeneral-regs-only`
  makes the compiler refuse any float.
- PE checksum: ld writes a correct one. NT treats 0 as "not checksummed"; the
  hazard is a stale non-zero checksum after post-link patching
  (`0xC0000221`). Recompute after any patch.
- `.bss` is fine (raw size 0, zero-filled by the loader).
- mingw `VIDEO_HW_INITIALIZATION_DATA` is 0x54 bytes = the XP size.
- Structural reference that works: **vmdispxp** (github f1nalspace/vmdispxp,
  GPL-2.0+, 2026-09) — ReactOS framebuf + bochs miniport built with mingw only.
  Read for structure; not copied (licence mix with the 3dfx Glide GPL code we
  port for SLI).

## Minimum driver surface

- Display DDI required: DrvEnableDriver, DrvEnablePDEV, DrvCompletePDEV,
  DrvDisablePDEV, DrvEnableSurface, DrvDisableSurface, DrvAssertMode,
  DrvGetModes, DrvDisableDriver; plus DrvSetPalette (8 bpp) and DrvEscape
  (QUERYESCSUPPORT, **OPENGL_GETINFO — XP's opengl32 finds the ICD by asking the
  display driver; unanswered, our MesaFX ICD is never loaded**, and the HWCEXT
  escapes). If any drawing call is hooked, hook all 14 (vmdispxp finding).
- Miniport: DriverEntry -> VideoPortInitialize; HwFindAdapter,
  HwInitialize, HwStartIO; PnP also wants HwGetPowerState, HwSetPowerState,
  HwGetVideoChildDescriptor. IOCTLs: QUERY_NUM_AVAIL_MODES, QUERY_AVAIL_MODES,
  QUERY_CURRENT_MODE, SET_CURRENT_MODE, RESET_DEVICE, MAP/UNMAP_VIDEO_MEMORY,
  SET_COLOR_REGISTERS; for MMIO/DirectDraw QUERY/FREE_PUBLIC_ACCESS_RANGES,
  SHARE/UNSHARE_VIDEO_MEMORY.
- INF: `$Windows NT$`, Class Display {4D36E968-...}, service type 1, start 1,
  error 0, LoadOrderGroup Video; `InstalledDisplayDrivers` (REG_MULTI_SZ),
  `VgaCompatible=0`. 3dfx sets `MaximumDeviceMemoryConfiguration=132`,
  `MaximumNumberOfDevices=4`. Unsigned install: `DRVUPDATE <hwid> <inf>` (agent
  1.85.1, `UpdateDriverForPlugAndPlayDevices` + INSTALLFLAG_FORCE).

## Hardware references

- bitsavers.org/components/3dfx: Banshee r1.1, Voodoo3 spec/databook,
  **Napalm (V4/V5) spec r1.13 and databook r1.12**.
- Linux tdfxfb (GPL): Banshee/V3/V5, 8-32 bpp, single chip only; pixclk limits
  270/300/350 MHz; 2X above max/2.
- xf86-video-tdfx (MIT): `CalcPLL` keeps **M in 1..56 (higher jitters)** and
  uses 2X above 135 MHz; **palette writes are retried with read-back up to 100
  times** (hardware quirk); `tdfx_sli.c` TDFXSetupSLI/TDFXDisableSLI.
- Multi-chip: Glide h5 `minihwc/dos_mode.c` / `lin_mode.c` (3dfx Glide GPL).

## Dev loop

1. QEMU + an overlay of the XP build VM, `-vga std` (Bochs VBE, 1234:1111) and
   our driver's bochs backend: proves the whole XDDM chassis, install, log
   ring, escapes, dump parsing and safety counter with no risk to hardware.
   `-debugcon` (port 0xE9) available for early logging.
2. (86Box v6.0 emulates Banshee/V3 register-level but NOT VSA-100; not
   installed; needs a fast host — ours qualifies.)
3. The real V5 6000 on `.124`.

Kernel debugging from Linux: rizin `winkd` over serial, or gdb against QEMU's
gdbstub (DWARF symbols from our build). Practical logging: ring + optional
polled UART (0x3F8) / debugcon (0xE9) mirror.

## Crash dumps (XP, 32-bit) — parse without WinDbg

`DUMP_HEADER32` (0x1000 bytes): 0x00 'PAGE', 0x04 'DUMP', 0x10
DirectoryTableBase, 0x18 PsLoadedModuleList, **0x28 BugCheckCode, 0x2C-0x38
params 1-4**, 0x5C PaeEnabled, 0x64 PHYSICAL_MEMORY_DESCRIPTOR {NumberOfRuns,
NumberOfPages, Run[]{BasePage, PageCount}}, 0x320 CONTEXT, 0x7D0
EXCEPTION_RECORD32, **0xF88 DumpType (1 full, 2 kernel, 4 triage)**, 0xFA0
RequiredDumpSpace, 0xFC0 SystemTime. Full dump pages from 0x1000; kernel dump
has `SUMMARY_DUMP32` at 0x1000 ('SDMP', 'DUMP', +0x0C HeaderSize = first page
file offset, +0x10 BitmapSize, +0x14 Pages, bitmap ~+0x20). XP minidumps are
PAGEDUMP files with DumpType 4 (~64 KB) and do NOT contain our ring.
**Scanning for the ring's magic sidesteps address translation**; entries are
128 bytes and 4 KB-page aligned groups stay contiguous.

`VideoPortRegisterBugcheckCallback` (XP SP1+, 0xEA only, <= 4000 bytes, via
VideoPortGetProcAddress); `KeRegisterBugCheckReasonCallback` covers others.
CrashControl: CrashDumpEnabled=2 (kernel), AutoReboot=1, Overwrite=1,
DumpFile=%SystemRoot%\MEMORY.DMP; pagefile on the Windows volume (**D: on
.124**); the dump becomes MEMORY.DMP only on the next successful boot.
Forced dump of a hang: `i8042prt\Parameters\CrashOnCtrlScroll=1` (PS/2),
right-Ctrl + ScrollLock x2 -> 0xE2.

## Watchdog (0xEA)

XP SP1+ times each thread inside the display driver (incl. miniport IOCTLs via
EngDeviceIoControl): debugger break, else try VGA recovery, else bugcheck 0xEA.
A HARD hang instead comes from spinning at raised IRQL, a PCI read the card
never completes (the likely wedged-VSA-100 shape), or spinning outside a DDI
call. **Bound every poll loop and log the register state before bailing.**
