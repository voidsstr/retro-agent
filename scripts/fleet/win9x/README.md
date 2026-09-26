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
| `reenum9x` | `CM_Reenumerate_DevNode` on the PCI bus (what agent 1.83.0's `PCIRESCAN` does) | `C:\RETRO_AGENT\REENUM.TXT` |
| `regdump9x <HKDD\|HKLM\|HKCC> <key> <out>` | recursive registry dump. Reads `HKEY_DYN_DATA`, the live devnode tree, which the agent's `REGREAD` cannot | the file you name |
| `wintext9x` | dumps every visible `#32770` dialog's controls: class, id, text, enabled, checked, rect. Lets you drive a wizard when 8-bpp screenshots are unreadable | `C:\RETRO_AGENT\WINTEXT.TXT` |
| `deskfix9x` | re-applies the registry display mode, restores the static palette and repaints everything - brought .243 back from a black/garbled desktop after GLQuake on its Voodoo 2 (link with `-lgdi32`) | `C:\RETRO_AGENT\DESKFIX.TXT` |
| `hash9x <file> [file ...]` | MD5 and size of each file, read 64 KB at a time. The agent's `DOWNLOAD` buffers a whole file in one heap block, which is no way to check an 80 MB pak on a 127 MB box. Proved agent 1.84.2's resume wrote Hexen II's paks correctly | `C:\RETRO_AGENT\HASH9X.TXT` |
| `ide9x identify` / `ide9x read <m|s> <lba> <count> <name>` | **read-only**, direct-port ATA IDENTIFY and READ SECTORS on the **secondary** IDE channel (170h-177h/376h), bypassing the BIOS and Win98's driver; never the primary channel, only commands ECh/20h, interrupts off while busy, NEW output files only. Identified the 80 GB disk the 1997 BIOS could not read on .243 | `C:\RETRO_AGENT\IDE9X.TXT`, `IDENT_M.BIN`, the named dump |
| `idewrite9x zero <serial> <lba> <count>` / `put <serial> <lba> <file>` / `flush <serial>` | the **only write-capable** tool here: direct-port ATA WRITE SECTORS (30h) and FLUSH CACHE (E7h) to the secondary MASTER only. Every command first IDENTIFYs the drive and refuses unless its serial is byte-for-byte the one you named; writes are bounded by the drive's own capacity, at most 256 sectors a run, `put` at most 128 sectors. Never kill it mid-run - create `C:\RETRO_AGENT\IDEW9X.STP` and it stops between runs. Shares the `retro_ide_secondary` mutex with ide9x. Formatted .243's 80 GB disk, ~3.6 MB/s | `C:\RETRO_AGENT\IDEW9X.TXT` |
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
