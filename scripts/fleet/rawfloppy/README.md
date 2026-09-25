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

## Recipe: an Award BIOS flash floppy (done 2026-09-24 for the EPoX EP-8RDA+)

1. **BIOS image** - The Retro Web keeps EPoX's BIOSes; the EP-8RDA+ (PCB 2.x)
   final is `8rda4729` (29 Jul 2004), 262,144 bytes, Award 6.00PG.
2. **Flasher** - The Retro Web driver 1408 is a collection of every AWDFLASH
   from 1.1 to 8.99. `AWD824F.exe` is AwardBIOS Flash Utility **V8.24F**, the
   version the nForce2 boards shipped with. The files are plain DOS MZ
   executables, not self-extractors - just rename to `AWDFLASH.EXE`.
3. **DOS** - FreeDOS 1.3 `FD13-FloppyEdition.zip` -> `144m/x86BOOT.img`.
   Copy that image, `mdeltree ::/FREEDOS` and `mdel` the installer's
   `SETUP.BAT` / `FDAUTO.BAT` / `FDCONFIG.SYS`, keeping `KERNEL.SYS` and the
   FreeDOS boot sector. Pull `COMMAND.COM` out of `::/FREEDOS/BIN` first -
   deleting the tree takes it with it.
4. `mcopy` in `COMMAND.COM`, `AWDFLASH.EXE`, the `.BIN`, an `FDCONFIG.SYS`
   (`SHELL=A:\COMMAND.COM A:\ /E:512 /P`), an `AUTOEXEC.BAT` banner and a
   `FLASH.BAT`. Everything **8.3 uppercase** - real DOS has no long names.
5. **Boot-test it before trusting it**, no hardware needed:

       qemu-system-i386 -m 32 -fda image.img -boot a -display none \
           -vnc :19 -monitor unix:mon.sock,server,nowait
       # then: screendump out.ppm  over the monitor socket

   This is worth the two minutes. A flash floppy that does not boot is found
   at the worst possible moment, in front of the machine with its case open.

**Do not auto-flash from `AUTOEXEC.BAT`.** The banner tells the operator to
type `FLASH`, so inserting the disk and powering on cannot by itself rewrite a
BIOS. `FLASH.BAT` saves the existing BIOS to `A:\OLDBIOS.BIN` (`/Sy`) before
programming - which needs the floppy left **write-enabled**.
