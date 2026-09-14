# BIOS recovery floppy — EPoX EP-8RDA+ (`.191`, the Voodoo 5 6000 box)

`.191` stopped booting on 2026-09-13 with **no video at all** — a corrupted
main BIOS. This directory is the blind bootblock-recovery floppy built to fix
it, the tool that writes it, and the reasoning behind every switch, because a
blind flash gives you one shot and no feedback.

    rawfd.c        writes a raw floppy image to a drive AND READS IT BACK
    build.sh       rebuilds the recovery image from scratch, reproducibly

## The board was failing progressively, and the evidence was already collected

Worth reading before assuming this was sudden. The box published a hardware
record on every agent startup, and comparing an old record with a live read
shows the BIOS losing its mind over time:

| | MAC of the onboard nForce NIC | reported CPU clock | RTC |
|---|---|---|---|
| published record | `00-04-61-2F-8F-84` | 1152 MHz | 2004-07-29 |
| live read, 2026-09-12 | `00-04-61-F7-25-84` | 1921 MHz | 2004-08-07 |

Same box, same IP, same Voodoo 5 6000. **On nForce boards the NIC's MAC is
stored in the BIOS**, so a MAC that changes between boots, a CMOS that keeps
reverting to the BIOS build date, and an FSB defaulting low are three symptoms
of one cause. The box also kept dropping off the network for a day before it
died, which read as "flaky machine" at the time.

**Both MACs are in the PXE `never_offer` list**, because a reflash may produce a
third one and an unblocked MAC on this fleet can be handed an install offer that
repartitions the disk. Re-check the MAC after recovery and block whatever it
comes back as.

## Which BIOS, and how we know

**`8RDA4729`** — md5 `f2e2164fa37139fb6e3ec1464329c3dc`, 262,144 bytes (2 Mbit).

Three independent reasons to trust it:

1. **Two separate uploads on TheRetroWeb are byte-identical.** The EP-8RDA+
   PCB 1.x page offers it as a `.bin` and the PCB 2.x page as a `.zip`; both
   extract to the same md5. That is two upload records agreeing, not one file
   taken on faith.
2. **The image says what it is.** It is an LHa container holding
   `6a61bpaa.BIN` — Award BIOS 6.0, where `6A61` is the nForce2 chipset code —
   and it contains the literal string **`Award BootBlock BIOS v1.0`**. So the
   recovery mechanism this whole procedure depends on is confirmed *from the
   firmware image*, not assumed.
3. **The version is almost certainly the one that was on the board.** EPoX
   names these `<year><month-in-hex><day>`: `8rda4729` = 2004-07-29. The dead
   box's own published record has `reported_at: 2004-07-29` — a cleared CMOS
   defaults to the BIOS build date. It is also the newest official release.

**PCB revision does not matter here**: the 1.x and 2.x pages list the *same*
BIOS version set. That was the biggest worry going in and it is resolved.

## The flasher: awdflash **8.24G**

From TheRetroWeb's AWDFLASH archive (161 versions). Chosen because it is
contemporary with the 2004 BIOS, it is nForce-MAC-aware (8.23K and later are;
8.22 and plain 8.23 are not), and its chip table covers **SST 49LF020/020A**,
the LPC part nForce2 boards use.

### The switches, read out of the binary — not from forums

    AWDFLASH 8RDA4729.BIN /py /sn /sb /cc /cd /cp /R

| switch | meaning (verbatim from the exe's own help) | why |
|---|---|---|
| `py` | Program Flash Memory | the point |
| `sn` | No Original BIOS Backup | the old BIOS is corrupt; a backup is worthless and a floppy write can fail |
| **`sb`** | **Skip BootBlock programming** | **the safety decision — see below** |
| `cc` `cd` `cp` | Clear CMOS / DMI / PnP(ESCD) after programming | the CMOS is part of what is broken |
| `R` | RESET System After Programming | there is no display, so nothing can press F1 |

**`/sb` is the important one.** We are *executing from the boot block*. Program
it and a failure mid-write leaves nothing to recover with — the next step after
that is a hardware programmer or a hot-swap. Skipping it means that if this
attempt fails, the boot block is still there and we can simply try again.

### Three pieces of widespread advice that are WRONG for this job

- **`/F` does not mean "force flash".** The exe says
  `F: Use Flash Routines in Original BIOS For Flash Programming`. On this board
  the original BIOS *is the corrupt thing*, so `/F` asks the flasher to rely on
  exactly what cannot be relied on. Web guidance found while researching this
  literally said "f = flash"; it is wrong and it is in the one place it would
  do harm.
- **`/tiny` destroys the BIOSLock signature.** The exe warns:
  *"To save BIOSLock signature, process BIOS update without /tiny switch,
  otherwise BIOSLock signature will be destroyed."* Not used.
- **`/QI` ("Qualify flash part number with source file") is an extra check**,
  not a force. Deliberately omitted — a corrupt main block can fail checks.

## The floppy

FreeDOS 1.3 `144m/x86BOOT.img` (freely redistributable) with the installer
removed, because its `FDCONFIG.SYS` opens a **10-second language menu** — fatal
when nobody can see the screen. Replaced with a single `SHELL=` line, no
`HIMEM`, no `EMM386`, so awdflash gets clean real mode.

    KERNEL.SYS    FreeDOS kernel        AWDFLASH.EXE  8.24G
    FDCONFIG.SYS  shell -> AUTOEXEC.BAT 8RDA4729.BIN  the BIOS
    CONFIG.SYS    same, as a fallback   COMMAND.COM   at the root as a fallback
    AUTOEXEC.BAT  the flash command     freedos/      BIN/COMMAND.COM

Rebuild it with `bash build.sh`. Write it with `rawfd.exe A: recovery.img`,
which writes and then **reads every sector back and compares** — on a disk
going into a machine that cannot report anything, "WriteFile returned success"
is not evidence.

## Doing the recovery

1. **Clear CMOS** with the jumper, battery out for a minute. The CMOS is
   suspect and a garbage CMOS can stop the board reaching the boot block.
2. Floppy in A:, PS/2 keyboard connected, no other removable media.
3. Power on and **leave it alone**. Expect: floppy light, a pause, more floppy
   activity as `AWDFLASH.EXE` loads, then ~30–60 s of flashing. **Nothing will
   appear on screen — that is normal.** Give it five minutes.
4. `/R` resets the board when it finishes. If the screen comes up, it worked.
5. It often takes **several attempts** to get the boot block to run; power-cycle
   and retry before concluding anything.

### If nothing ever happens

No floppy activity at all across several attempts means the **boot block itself
is gone**, and no floppy can fix that — it needs a hardware programmer
(CH341A) or a hot-flash on another board. That is the honest failure mode and
it is worth recognising early instead of retrying for an hour.

### Afterwards

- **Check the MAC** (`HWPROFILE`) and add it to PXE `never_offer` if it changed.
- The MAC can be written deliberately:
  `AWDFLASH 8RDA4729.BIN /py/sn/nvmac:000461F72584/wb` — but note `/wb`
  programs the boot block, so only do that **once the board boots normally**,
  never as part of a blind recovery.
