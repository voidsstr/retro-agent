# rawfloppy - write a bootable floppy image from a fleet Windows box

## Why this exists

**Windows NT cannot make a bootable floppy from the command line.** XP's
`format.com` dropped `/s` (transfer system files) and the only remaining route
is Explorer's "Create an MS-DOS startup disk" checkbox, which needs a human at
the screen. `format a:` therefore gives a *formatted* floppy that will not
boot, and nothing says so until the target machine says `Non-System disk`.

So a bootable floppy is built as a whole **1,474,560-byte image on the Linux
host** - boot sector, FAT, files, all of it - and written to the physical disk
sector-for-sector by this tool. The floppy drive the fleet has and the
motherboard being flashed need not be the same machine.

## Usage

    rawfloppy write  A: image.img    write the image to the disk
    rawfloppy verify A: image.img    read the disk back and compare
    rawfloppy read   A: out.img      dump the disk to a file

Exit code 0 on success. Every failure names the Windows error **and the byte
offset**, because a write that stops half way must not look like a write that
finished - `dir a:` will happily list files off a disk whose last track never
landed.

**Always `verify` after `write`.** It reads back through the drive, not through
the OS cache of what was written, and it is the only thing that proves the
media took the bytes. Floppies rot; this fleet's stock is 20+ years old.

## Building

    i686-w64-mingw32-gcc -O2 -s -o rawfloppy.exe rawfloppy.c -lkernel32 -luser32

## DO NOT invent a BIOS flash floppy with this tool - use `provisioning/bios-recovery/`

**This README used to carry a "recipe" for building an Award flash floppy, and
following it hung an EPoX EP-8RDA+ mid-flash on 2026-09-24.** The recipe was
written from scratch while a vetted one for that exact board already existed in
this repo, unread, at [`provisioning/bios-recovery/`](../../../provisioning/bios-recovery/README.md).
The recipe is deleted rather than corrected, because the correct one is not
here.

The difference that mattered was one switch:

| | this README's old recipe | `provisioning/bios-recovery/build.sh` |
|---|---|---|
| flasher | AWDFLASH 8.24F | **8.24G** - nForce-MAC aware, SST 49LF020 in its chip table |
| boot block | *nothing* - left to awdflash's default | **`/sb` - Skip BootBlock programming** |
| backup | `/Sy` to the floppy | `/sn` - no backup |
| invocation | operator types `FLASH` | auto-runs from `AUTOEXEC.BAT` (blind recovery has no screen) |

**`/sb` is the one that turns a failed flash into a retryable one.** The
flasher is executing *out of the boot block*; if the boot block is in the set
of blocks being rewritten when the board hangs, there is nothing left to
recover with and the next step is a hardware programmer. `provisioning/bios-recovery/README.md`
reasons this out switch by switch, from the flasher binary's own help text,
and explains why three pieces of common web advice (`/F`, `/tiny`, `/QI`) are
wrong for the job.

**The general lesson, which is the one this project keeps paying for:** search
the fleetbook and the repo *before* building, not after. `retro_fleetbook.py
search` and a `grep -ril` for the board name would each have found the existing
work in seconds.

So: this tool writes images. **What goes in the image, for a BIOS flash, is
decided somewhere else.**
