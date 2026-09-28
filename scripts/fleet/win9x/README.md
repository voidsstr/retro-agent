# Win9x helper tools — no C runtime

`fleet9x.exe` is a small Win32 GUI program for Windows 95/98 boxes:

    fleet9x attrib <path> <hex FILE_ATTRIBUTE_* mask>   # e.g. MSDOS.SYS 80, then back to 27
    fleet9x reboot                                      # forced reboot: kill console vetoes, ExitWindowsEx, keep pumping

Each run appends one line to `C:\RETRO_AGENT\FLEET9X.TXT`, so the result can be read back with `DOWNLOAD`.

Build (it must be `-nostdlib`, see below):

    i686-w64-mingw32-gcc -O1 -march=i586 -mwindows -nostdlib -e _start@0 \
        -o fleet9x.exe fleet9x.c -lkernel32 -luser32 -s

Run it through the agent: `EXECW 30 C:\RETRO_AGENT\FLEET9X.EXE attrib ...`, or `LAUNCH` for `reboot`.

## The other tools (all found necessary on .243, 2026-09-24)

| tool | what it does | output |
|---|---|---|
| `pci9x [out] [noio]` | read-only PCI bus-0 config scan (mechanism #1: it only ever writes 0xCF8, never 0xCFC); reports each function's command register and BARs, with a verdict on any 3dfx BAR0 | `C:\RETRO_AGENT\PCI9X.TXT` |
| `devctl9x disable\|enable\|status\|persistoff\|persiston <id>... \| @file` | live `CM_Disable_DevNode`/`CM_Enable_DevNode`, or (`persist*`) ONLY `HKLM\Enum\<id>` ConfigFlags 01/00 + `RegFlushKey`, touching no devnode - for a box that freezes before a lazily flushed REGEDIT change reaches the disk (.243 with its NEC USB card, 2026-09-27). Ids from a file because COMMAND.COM caps a line at ~127 chars | `C:\RETRO_AGENT\DEVCTL.TXT` |
| `reenum9x` | `CM_Reenumerate_DevNode` on the PCI bus (what agent 1.83.0's `PCIRESCAN` does) | `C:\RETRO_AGENT\REENUM.TXT` |
| `regdump9x <HKDD\|HKLM\|HKCC> <key> <out>` | recursive registry dump. Reads `HKEY_DYN_DATA`, the live devnode tree, which the agent's `REGREAD` cannot | the file you name |
| `usb9x [out]` / `usb9x watch <secs> [out]` | **read-only** UHCI probe: each controller's PCI command/status and interrupt line, USBCMD/USBSTS (running, halted, host-system-error), whether the frame number advances, and PORTSC1/2 - is a device **connected**, is the port **enabled**; plus the PIIX PIRQA-D routing, the ELCR (a PCI IRQ must be level) and the IMR. `watch` logs every register change at millisecond resolution. Only writes 0xCF8, restored; never a UHCI register | `C:\RETRO_AGENT\USB9X.TXT`, `USBWATCH.TXT` |
| `wintext9x` | dumps every visible `#32770` dialog's controls: class, id, text, enabled, checked, rect. Lets you drive a wizard when 8-bpp screenshots are unreadable | `C:\RETRO_AGENT\WINTEXT.TXT` |
| `deskfix9x` | re-applies the registry display mode, restores the static palette and repaints everything - brought .243 back from a black/garbled desktop after GLQuake on its Voodoo 2 (link with `-lgdi32`) | `C:\RETRO_AGENT\DESKFIX.TXT` |
| `hash9x <file> [file ...]` | MD5 and size of each file, read 64 KB at a time. The agent's `DOWNLOAD` buffers a whole file in one heap block, which is no way to check an 80 MB pak on a 127 MB box. Proved agent 1.84.2's resume wrote Hexen II's paks correctly | `C:\RETRO_AGENT\HASH9X.TXT` |
| `ide9x identify` / `ide9x read <m|s> <lba> <count> <name>` | **read-only**, direct-port ATA IDENTIFY and READ SECTORS on the **secondary** IDE channel (170h-177h/376h), bypassing the BIOS and Win98's driver; never the primary channel, only commands ECh/20h, interrupts off while busy, NEW output files only. Identified the 80 GB disk the 1997 BIOS could not read on .243 | `C:\RETRO_AGENT\IDE9X.TXT`, `IDENT_M.BIN`, the named dump |
| `idewrite9x zero <serial> <lba> <count>` / `put <serial> <lba> <file>` / `flush <serial>` / `hpa <serial> <maxlba>` | the **only write-capable** tool here: direct-port ATA WRITE SECTORS (30h), FLUSH CACHE (E7h) and the Host Protected Area pair READ NATIVE MAX (F8h) / SET MAX ADDRESS (F9h, kept across power cycles) on the secondary MASTER only. Every command first IDENTIFYs the drive and refuses unless its serial is byte-for-byte the one named; `hpa` accepts only the native max (undo) or whole 16x63 cylinders and reads the new size back. **Both IDE tools refuse to run while Windows owns the channel** (live devnode tree: MF\CHILD0001 at Problem 0, or any devnode on &CHILD0001&) - see below. Never kill it mid-run - create `C:\RETRO_AGENT\IDEW9X.STP`. Shares the `retro_ide_secondary` mutex with ide9x | `C:\RETRO_AGENT\IDEW9X.TXT` |
| `cmosw9x postskip on\|off` / `none` / `restore` / `show` | the Compaq Deskpro 2000's CMOS from Windows: **`postskip on`** sets 2Dh bit 3 ("POST Error Handling: skip F1 message" - POST shows an error and boots on instead of waiting for F1); `none`/`restore` set 1Bh (the secondary IDE master type). Only 1Bh/2Dh/2Eh/2Fh can be changed; the checksum (sum of 10h-2Dh, the ROM's formula) is computed from the live bytes and must be valid before any change; writes are paced with `in al,84h`; the whole bank is re-read (0Ah/0Bh included) and put back to the snapshot on any difference. **Since agent 1.86.0 the agent re-asserts `postskip on` itself at every start** (`agent/src/postskip.c`, same guards, this ROM only), so the tool is needed only for `off`, `none`/`restore` and diagnosis. **Known limit (found by the 1.86.0 reviews): this tool's repair path restores every differing register from ONE snapshot read and compares 0Ah's self-toggling UIP bit** - so run it only with nothing else touching the RTC (the agent's `clockfix` finished, no time change in progress) and read `CMOSB.BIN`/`CMOSA.BIN` afterwards; the agent's loop (`agent/shared/postskip.h`: double reads, UIP masked, logged-byte attribution) is the model to port if it is used again | `C:\RETRO_AGENT\CMOSW9X.TXT`, `CMOSB.BIN`/`CMOSA.BIN` |
| `disk9x info` / `disk9x read ...` | read-only VWIN32 INT 13h reader - **but VWIN32's INT 13h does not serve hard disks on 9x** (measured: even 80h, the boot disk, is refused), so it only proves that route is closed | `C:\RETRO_AGENT\DISK9X.TXT` |
| `agentswap9x` | installs `retro_agent_new.exe` over a RUNNING agent (Win9x cannot replace a running exe). LAUNCH it, then send `QUIT`; it swaps, starts the new build, and rolls back if that build is not still running after 25 s | `C:\RETRO_AGENT\AGENTSWAP.TXT` |

All of them build with the same command (add `-ladvapi32` for regdump9x). GUI-subsystem exes are not waited for by `EXEC` on 9x, so poll `PROCLIST` until the process is gone, then `DOWNLOAD` the output.

## Why fleet9x exists (found on .243, Win98 SE, 2026-09-24)

* **Agents older than 1.82.1 cannot reboot a Win9x box.** `REBOOT` answered `OK` and did nothing: `CreateThread` with a NULL thread id fails on 95/98. Fixed in 1.82.1. Until a box runs that build, `fleet9x reboot` is the remote route.
* **The agent cannot write a read-only file**, and running DOS `ATTRIB` through `EXEC` starts a DOS VM, which is the pattern that has killed this single-threaded agent before. `fleet9x attrib` is plain Win32.
* **Build every Win9x helper without a C runtime.** On .243 the first, msvcrt-linked build of this tool sat in `PROCLIST`, wrote nothing, and left a `#32770` dialog named after itself. That orphaned dialog then **blocked a later `ExitWindowsEx`** until someone pressed Enter on it. The CRT-free rebuild never did this. The cause was never established: "an msvcrt export Win98 lacks" was checked and is wrong for two similar probes. So build `-nostdlib`, keep buffers larger than 4 KB static (otherwise `__chkstk_ms` gets linked in), and check `WINLIST` for `#32770` after running anything new.

## Caution

`reboot` is a forced reboot. On .243 on 2026-09-24 it went through (the agent's connection was reset as the session ended), but the machine did not come back without a person, exactly as a forced reboot did on 2026-08-31. The cause is not known: it may be the Win98 shutdown, or the new Voodoo 2, which hangs the PCI bus whenever software probes it. Arm the PXE hold first (`scripts/pxe/pxe_server.py --arm <mac>`) and do not use it on a box nobody can reach.

## Identifying a disk Win98 cannot read (.243, 2026-09-25)

A second IDE disk showed at POST, but `C:\WINDOWS\IOS.LOG` said `ESDI BIOS read
failure` and Windows gave it no letter (the 1997 BIOS cannot address an 80 GB
disk). What does NOT work, measured: VWIN32's INT 13h (refuses every hard disk,
80h included), and **`FDISK /STATUS` - it stalled the whole box for about five
minutes probing the disk through the BIOS, and the agent with it**. What works:
`ide9x` (direct ports, no BIOS), then `ntfsprobe.py` on the host, which parses an
NTFS volume from raw sectors fetched with `ide9x read` (boot sector, $MFT
runlist, fixups, $INDEX_ROOT/$INDEX_ALLOCATION) and can `--list` a folder or
`--cat` a file. Both tools were adversarially reviewed before first use and are
pinned by `tests/python/test_ide9x_readonly.py` / `test_disk9x_readonly.py`.

### Formatting it from Windows 98 anyway (2026-09-25)

With no BIOS path and no Windows driver, the disk was formatted from the host:
`mkfs.fat` (the reference implementation, then `fsck.fat -n`) builds an image of
exactly the partition, only its non-zero sectors are shipped, and `idewrite9x`
zeroes the metadata region and writes those sectors, the MBR last:

- **FAT32, 32 KB clusters**, 64 reserved sectors, FATs of 19136 sectors, OEM
  name patched to `MSWIN4.1` (what Win98's own FORMAT writes).
- One partition at LBA 63, **cylinder-aligned** (156,296,322 sectors, ending at
  cylinder 9728), which is what Win98 FDISK would have made.
- Written as **type 1Ch (hidden FAT32 LBA) first**. A visible 0Ch partition on
  a disk the BIOS mis-addresses risks IO.SYS mounting it through INT 13h at
  boot, or Windows falling back to MS-DOS compatibility mode for it, and either
  would write through the BIOS's wrong geometry. Flip it to 0Ch only once
  `ESDI_506` has claimed the disk natively.
- Also zeroed: the old MBR gap, the old NTFS $MFT (64 MB), $MFTMirr and backup
  boot sector, so nothing finds a stale NTFS volume. The rest of the old data
  is simply free space now.
- Verified by reading back every written sector and both edges of every zeroed
  range with `ide9x read` (24/24).

### Why Windows never took the disk, and the fix (2026-09-26)

Established by disassembling the box's own `ESDI_506.PDR` (twice, independently)
and its Compaq BIOS ROM:

- POST auto-typed the Seagate as **Compaq type 68**, whose logical-heads byte is
  **00** (256 heads, overflowed). The BIOS's INT 13h CHS validator
  (F000:84A1) therefore rejects every read above head 0.
- ESDI_506 matches the disk to BIOS unit 81h by the MBR dword at 0xDC, then
  verifies with an INT 13h AH=02 read at head H-1. That read fails, ESDI logs
  `ESDI BIOS read failure` and tears down the **whole secondary channel**
  (Config Manager Problem 10).
- **`NoCMOSorFDPT` does not help.** It is a 1-byte REG_BINARY read from the
  channel's HARDWARE key, but on a Compaq the secondary channel reads CMOS 1Bh
  directly and still verifies through the BIOS.
- **The fix is CMOS 1Bh = 00** (`cmosw9x none`): the BIOS then creates no unit
  for the disk, and ESDI claims it from IDENTIFY with LBA28 and no BIOS read.
  Keep the partition type 1Ch until a **cold** POST is proven to leave 1Bh at
  00 (POST auto-typed the drive once already); only then flip it to 0Ch.
- Pre-flight markers are written for the final check: LBA 156,296,384 (end of
  partition) and 78,148,223 (~40 GB), each a readable ASCII tag.

## How .243's second disk came online (2026-09-27)

The Compaq BIOS translates by doubling heads while cylinders > 1024, so any
drive reporting more than 8191 cylinders overflows to 256 heads and every BIOS
read above head 0 fails - which also makes ESDI_506 tear down the channel.

1. `idewrite9x hpa 5JVQM4FT 8256527`: the Seagate now REPORTS 8,256,528
   sectors, default CHS 8191/16/63 (BIOS translation 1023/128/63). Kept across
   power cycles; the 80 GB behind it comes back with `hpa <serial> 156301487`.
   Boundary checked: LBA 8,256,527 reads, 8,256,528 is refused (IDNF).
2. New FAT32 of 4.2 GB (4 KB clusters, 32 reserved, backup boot at 6, OEM
   MSWIN4.1), one partition LBA 63 .. 8,241,407 = cylinder 1021 (INT 13h AH=08
   reports two cylinders fewer than the table), type **0Bh** (CHS FAT32), not
   0Ch: if a power-on ever brings the broken geometry back, DOS's CHS read
   fails cleanly on the BIOS head check instead of entering the extended-read
   path that divides by the broken heads byte.
3. CMOS 1Bh = 00 (`cmosw9x none`). **A warm POST does NOT auto-type the drive**
   (measured), so after a warm reboot the BIOS has no unit 81h and Windows'
   ESDI_506 claims the disk natively: `ESDI\GENERIC_IDE__DISK_TYPE00_\MF&CHILD0001`,
   Problem 0. **A power-on auto-types** it from IDENTIFY; with 8191 cylinders
   that should give a valid 1023/128/63 BIOS unit, so real DOS sees it too -
   not yet observed.
4. The SB16's own IDE interface (ISAPNP\CTL0024_DEV0001) asks for exactly
   170h-177h/376h/IRQ15 and is disabled (ConfigFlags 01) so it can never take
   the channel.
5. **Never run ide9x/idewrite9x once Windows owns the channel.** Doing exactly
   that (an `ide9x identify` in a check script) left the driver waiting on a
   masked interrupt; the agent blocked on D: and died with nobody at the box.
   Both tools now refuse (exit 7) when the live devnode tree shows the channel
   or a disk on it.

## A USB mouse on .243 (2026-09-28): connected, never enabled - restart the controller

The VIA VT83C572 card's root hub read problem 0 and **nothing** appeared below it
- no device, not even "Unknown Device" - with a wireless mouse plugged in.
`usb9x` showed why: the controller was running (frames advancing, not halted),
PORTSC2 = `01A1` (**connected, low-speed, enabled = 0**) and its connect-change
bit already clear. Win98's hub driver acts on change bits, so a device whose
change was consumed without the port ever being reset is ignored for good; a
root-hub disable/enable (`devctl9x`) changed nothing (`usb9x watch` saw no port
activity at all). **A controller disable/enable did it**: the controller reset
re-detects the device with a fresh connect-change, and the hub enumerated it at
once.

The receiver (`USB\VID_30FA&PID_1040`) is **composite** (device class 0):
- the parent matches `USB\COMPOSITE` in `USB.INF` only by a compatible id, so
  Win98 opens the Add New Hardware Wizard instead of installing silently. Enter
  through it; then answer **No** to "System Settings Change" (Yes is the
  default and reboots the box) - no restart was needed, every node came up;
- its two interfaces then install `HIDDEV.INF` (USB Human Interface Device) and
  `MSMOUSE.INF` / keyboard (HID-compliant mouse, HID-compliant keyboard,
  consumer and system control) - another wizard;
- Setup copies from `SourcePath` (`C:\WINDOWS\OPTIONS\CABS`): stage
  `HIDCI.DLL`, `MOUSE.DRV` and `MSMOUSE.VXD` there from `C:\WINDOWS\SYSTEM`
  first (they were missing, so it would prompt for the CD);
- **`hidserv.exe`** (the media-key service for the consumer-control collection)
  is on no share and in no pack here - press **Skip File** (`wintext9x` read
  the prompt; `UIKEY ALT+S`). The mouse and keyboard are unaffected.

Drive the wizards with `UIKEY RETURN` only while `WINLIST`'s top window is a
Setup dialog; 8-bpp screenshots on this box are unreadable, `wintext9x` is not.
