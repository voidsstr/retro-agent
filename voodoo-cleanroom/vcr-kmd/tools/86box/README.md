# The 86Box Voodoo3 test bed

A second test bed for vcr-kmd, next to the QEMU VM (`tools/qemu/`): **86Box
v6.0 emulating a real 3dfx Voodoo3 3000 AGP**, register for register - the
VGA/video processor, the 2D engine, the command FIFO and the 3D pipeline. The
QEMU VM exercises the chassis on a Bochs VGA; this one runs the driver's
**Voodoo** paths, and anything that wedges the chip wedges an emulator that a
script restarts, not a box that needs a person at the power button.

What it is not: a VSA-100. 86Box has Voodoo Graphics, Voodoo 2, Banshee and
Voodoo3 - no Voodoo 4/5. The VSA-100-only work (SLI, the ICS307 clock, AA)
still needs `.124`; everything the Banshee/Voodoo3/VSA-100 share (the mode set
path, the 2D engine, the 3D register set, DDC) is testable here.

## The machine

`86box.cfg`: ASUS P3B-F (440BX, PIIX4E, ACPI), Pentium II 450 (86Box has no
SSE, so no Pentium III), 512 MB, Voodoo3 3000 AGP, RTL8139C+ on SLiRP with
the guest's agent forwarded to **`0.0.0.0:19920`**, a 3.5" floppy drive.

| file | what |
|---|---|
| `setup.sh [disk.img]` | fetch the 86Box v6.0 AppImage (extracted) and the ROM set into `$VCR86_DIR` (default `~/retro-vm/86box`), install `86box.cfg`, link the disk |
| `prep-xp-disk.py <ip> --port 19910` | make the QEMU build VM's XP bootable on this board (below) - run BEFORE copying its disk |
| `run-86box.sh` | start it headless on its own Xvfb (`:21`): `systemd-run --user --unit=vcr86box tools/86box/run-86box.sh` |
| `restart-guest.sh` | shut Windows down through the agent, restart the emulator - `deploy_box.py --reboot-cmd` |
| `86box-shot.py [out.png]` | capture the emulated screen (what the Voodoo3 SCANS OUT, not what GDI thinks it drew) |
| `86box-key.py KEY...` | type into the emulated machine via XTEST (`F1`, `F8`, `Return`, `text:...`) - BIOS prompts, the boot menu, Safe Mode |

Install our driver on the Voodoo3:

```bash
python3 tools/deploy_box.py install 127.0.0.1 --port 19920 \
    --hwid 'PCI\VEN_121A&DEV_0005' --rollback-dir "" \
    --reboot-cmd tools/86box/restart-guest.sh --evidence evidence/86box_v3/<run>
```

and run the labs against it: `tools/lab_run.py gdilab 127.0.0.1 --port 19920`,
`tools/ddlab_run.py 127.0.0.1 --port 19920 blt --res 800x600 --bpp 16`,
`tools/d3dprobe_run.py 127.0.0.1 --port 19920 render`.

## The reference driver is already on it

XP ships its own Voodoo3 driver (`3dfxvs2k.inf`, Microsoft/3dfx 5.1.2001.0,
`3dfxvs.dll` 5.0.2489.28) and binds it on the first boot on this board. That is
the bed's golden: DirectDraw and Direct3D on the same emulated card, measured
by the same labs. `evidence/86box_v3/inbox_*`:

| lab (800x600x16) | in-box driver | vcr-kmd (2026-09-26) |
|---|---|---|
| ddlab flip | 60.9 flips/s, 0 mismatch | 62.3 flips/s, 0 mismatch |
| ddlab blt | 1185 blts/s, 77.6 Mpix/s, 0 bad | 2349 blts/s, 153.9 Mpix/s, 0 bad (copy, fill, scroll, colour key) |
| gdilab | - | 0 bad (fills, ROPs, copies, 4 scroll directions, clip, engine/CPU interleave) |
| d3dprobe render | 26/26, windowed and fullscreen | no Direct3D HAL yet |

At 24 bpp the in-box driver switches DirectDraw acceleration off entirely
(`DDCAPS_NOHARDWARE`); at 16 and 32 bpp it is a full DX7 HAL.

## The disk image

The XP build VM's disk (`xp3.qcow2` through the vcr-kmd VM's overlay),
flattened: `qemu-img convert -O raw overlay.qcow2 xp.img` (sparse, ~29 GB used
of 60). Shut the VM down first. Two things differ between what QEMU installed
and what this board needs, and `prep-xp-disk.py` fixes both in the running VM
beforehand - each is otherwise a boot failure with nothing on the screen:

- **the kernel and HAL.** `-smp 2` gave the install the ACPI multiprocessor
  pair; this board has one CPU and no I/O APIC. The uniprocessor ACPI (PIC)
  pair from `sp3.cab` goes next to the originals (`ntosup.exe`,
  `halacpi.dll`) and a first `boot.ini` entry selects it with `/kernel=`
  `/hal=`. The QEMU VM boots that entry too (it runs on one CPU then).
- **the IDE controller.** PIIX4's IDE (8086:7111) is not in the critical
  device database of an install that only saw QEMU's PIIX3 -
  INACCESSIBLE_BOOT_DEVICE. Same driver, `intelide`.

## Traps (each cost time)

- **86Box's status register does not count queued 2D writes as busy.**
  `status[9]` covers started work and the command FIFO; a 2D operation written
  straight to the registers sits in the emulator's FIFO ring meanwhile, and only
  the FIFO free count (`status[4:0]`) shows it. A sync that waits for busy
  alone returns early. The driver's sync waits for busy clear **and** the FIFO
  back at its empty free count - correct on silicon as well (gdilab's
  engine/CPU interleave went from 170 bad to 0).
- **`vidCurrentLine` is not emulated** - `GetScanLine` reads garbage here. The
  vertical retrace bit (`status[6]`, CLEAR in retrace) is.
- **Xvfb at depth 24 packs 3 bytes a pixel** while `xwd` reports a 4-byte row
  pitch. Decoding it as 32-bit turns the whole screen into rainbow columns -
  86Box's own toolbar included, which is the tell that it is the capture and not
  the driver. `86box-shot.py` decodes by `bits_per_pixel`.
- **The BIOS halts on "Floppy disk(s) fail"** (and CMOS defaults, first boot)
  and waits for F1 unless a floppy drive is configured - `fdd_01_type = 35_2hd`.
- **86Box rewrites `86box.cfg` when it exits** - edit it with the emulator stopped.
- The SLiRP forward binds **0.0.0.0**, not 127.0.0.1 (86Box has no bind
  option). The agent secret is the fleet's LAN convention; nothing else listens.
- The emulator runs at ~100 % of a real Pentium II 450: boot to a PING-able
  agent is ~2 minutes, and `deploy_box.py`'s preflight should not start before.
