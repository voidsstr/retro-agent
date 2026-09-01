<!-- GENERATED FILE - DO NOT EDIT BY HAND -->
# Fleet hardware inventory

**This file is generated. Do not hand-edit it** - every change is overwritten
the next time `scripts/fleet/inventory.py` runs. Each machine publishes its own
record (`agent/src/hwpublish.c`, on every agent startup) to
`\\192.168.1.122\files\Utility\Retro Automation\fleet-inventory\<host>.json`,
and this document is nothing but those records rendered.

To correct something here, fix the machine or the probe, not the file. Per-box
prose that a probe cannot discover - the traps, the dual-boot layouts, "this
box answers slowly" - lives in `scripts/fleet/fleet-roster.txt` and in
CLAUDE.md's *Known Machines* section, which deliberately holds **no** measured
field.

Regenerate with:

```bash
python3 scripts/fleet/inventory.py
```

*Rendered 2026-09-01 17:45 from records in `/mnt/retro-share/Utility/Retro Automation/fleet-inventory`.*

> **Every field here is a SNAPSHOT, stamped per box.** A box republishes on
> every agent startup, so the fleet's records refresh themselves whenever the
> machines are restarted - but between restarts this file says what each box
> *said*, not what it *is*. Read the Measured column before quoting anything,
> and if a figure matters right now, restart that box's agent and regenerate.
>
> **A stale or missing record is not an outage.** The fleet is powered on
> demand - the retro machines are deliberately kept off - so at any moment
> several boxes legitimately carry old data. `stale` means *re-measure before
> trusting this*; `never seen` means that box has never published at all.
> Staleness is judged by when the record landed on this host, not by the retro
> box's own clock, which on hardware this old is frequently years out.


## Summary

Every row is **what that box said about itself at the time in the Measured column** - not what it is now. A machine re-publishes on every agent startup, so the way to refresh any of this is to restart the box's agent and re-run the renderer.

| IP | Hostname | State | Measured | Agent | CPU | RAM | Display GPU | OS |
|----|----------|-------|----------|-------|-----|-----|-------------|----|
| 192.168.1.123 | NSC-B20C188E96D | current | 9 min ago | 1.79.4 | AMD Athlon(tm) 64 Processor 4000+, 2402 MHz | 2047 MB | ATI Radeon HD 3850 AGP (`1002:9515`), 512 MB, sm3.0 | Windows XP |
| 192.168.1.124 | NSC-AB862B3CF23 | current | 1 min ago | 1.79.4 | AMD Athlon(tm), 1152 MHz | 511 MB | 3dfx Voodoo5 (`121A:0009`), 32 MB, fixed | Windows XP |
| 192.168.1.133 | P3-DUAL | current | 1 h ago | 1.79.4 | GenuineIntel, 701 MHz x2 | 255 MB | NVIDIA GeForce4 Ti 4600 (`10DE:0250`), 128 MB, sm1.x | Windows XP |
| 192.168.1.143 | 1GHZ | current | 1 h ago | 1.79.4 | AMD Athlon(tm) Processor, 1000 MHz | 511 MB | NVIDIA GeForce 6800 (`10DE:0041`), 128 MB, sm3.0 | Windows XP |
| 192.168.1.145 | DELL | current | 1 h ago | 1.79.4 | Intel(R) Core(TM) i5-2400 CPU @ 3.10GHz, 3092 MHz x4 | 3316 MB | NVIDIA GeForce 8400GS (`10DE:10C3`), 512 MB, sm3.0 | Windows XP |
| 192.168.1.171 | NSC-5B996B81319 | current | 1 days ago | 1.78.0 | Intel(R) Pentium(R) 4 CPU 2.80GHz, 2793 MHz | 509 MB | Intel(R) 82865G Graphics Controller (`8086:2572`), 96 MB, fixed | Windows XP |
| 192.168.1.240 | USER-41EA3B3330 | current | 1 h ago | 1.79.4 | AMD Athlon(tm) 64 Processor 3300+, 2403 MHz | 1022 MB | RADEON X800 Series (`1002:4A4B`), 256 MB, sm2.0 | Windows XP |
| 192.168.1.246 | ADMIN-PC | current | 1 h ago | 1.79.4 | Intel(R) Core(TM) i5-2400 CPU @ 3.10GHz, 3093 MHz x4 | 3317 MB | AMD Radeon HD 5450 (`1002:68F9`), 512 MB, sm3.0 | Windows 7 |

---

## 192.168.1.123 - NSC-B20C188E96D

**As this box reported itself at 2026-09-01 17:36** (9 min ago). Every value below is that snapshot; restart its agent to take a fresh one.

> Prone to being left at 640x480 by a game that exits without restoring - the Display mode row shows both, and the PERSISTED one is what the box is configured to be.

| field | value |
|-------|-------|
| CPU | AMD Athlon(tm) 64 Processor 4000+, 2402 MHz |
| CPU id | family 15 model 39 stepping 1, vendor `AuthenticAMD` |
| Instruction set | fpu, mmx, cmov, 3dnow, sse, sse2, sse3 |
| RAM | 2047 MB |
| Display GPU | ATI Radeon HD 3850 AGP (`1002:9515`), 512 MB, sm3.0 |
| GPU driver | 8.970.100.0 (4-24-2013) |
| Display mode | 1920x1080 (registry) |
| OS | Windows XP Service Pack 3 (5.1.2600) |
| DirectX | 4.09.00.0904 |
| Disks | C: 179/238 GB free |
| Agent | 1.79.4 (the version that WROTE this record, at 2026-09-01 17:36) |
| Profile hash | `e633518d4747e4f2` |

**No 3dfx silicon.** `Enum\PCI` carries no `VEN_121A` key, and a physically fitted card enumerates there even with no driver bound - so this is the decisive read, not merely "undriven".

Network: 192.168.1.123 - MAC 00-13-D4-A4-A4-13

---

## 192.168.1.124 - NSC-CABE14B7486

**As this box reported itself at 2026-09-01 17:44** (1 min ago). Every value below is that snapshot; restart its agent to take a fresh one.

> Dual-boot: XP on D:, Win98 on C:, and games live on both volumes. Console account "voidsstr". Voodoo 3 removed 2026-08-11. NO SSE2 - see the Instruction set row: UT99 469e and Halo are refused here, and a 436 client cannot join the fleet 469e server, so this box has no route to UT99 multiplayer at all.

| field | value |
|-------|-------|
| CPU | AMD Athlon(tm), 1152 MHz |
| CPU id | family 6 model 10 stepping 0, vendor `AuthenticAMD` |
| Instruction set | fpu, mmx, cmov, 3dnow, sse |
| RAM | 511 MB |
| Display GPU | 3dfx Voodoo5 (`121A:0009`), 32 MB, fixed |
| GPU driver | 5.1.2001.0 (6-6-2001) |
| Display mode | 800x600 (registry) |
| OS | Windows XP Service Pack 3 (5.1.2600) |
| DirectX | 4.09.00.0904 |
| Disks | C: 113/116 GB free |
| Agent | 1.79.4 (the version that WROTE this record, at 2026-09-01 17:44) |
| Profile hash | `bdde68b6c7005a40` |

**3dfx silicon: 1 card.** From the PCI enumerator (`Enum\PCI`, `VEN_121A`), which is the only source a `Class=MEDIA` Voodoo 2 cannot hide from - it appears in no display-class scan at all.

- `VEN_121A&DEV_0009&SUBSYS_0001121A&REV_01` - 3dfx Voodoo5 (1 instance)

Network: 192.168.1.124 - MAC 00-D0-B7-40-96-A9

---

## 192.168.1.133 - P3-DUAL

**As this box reported itself at 2026-09-01 16:03** (1 h ago). Every value below is that snapshot; restart its agent to take a fresh one.

> Dual-socket. The Voodoo5 6000 is PHYSICALLY GONE - Enum\PCI carries no VEN_121A key at all. NO SSE2, so UT99 469e and Halo are refused and there is no route to UT99 multiplayer here.

| field | value |
|-------|-------|
| CPU | GenuineIntel, 701 MHz x2 |
| CPU id | family 6 model 8 stepping 1, vendor `GenuineIntel` |
| Instruction set | fpu, mmx, cmov, sse |
| RAM | 255 MB |
| Display GPU | NVIDIA GeForce4 Ti 4600 (`10DE:0250`), 128 MB, sm1.x |
| GPU driver | 6.14.10.9371 (10-22-2006) |
| Display mode | 1280x1024 (registry) |
| OS | Windows XP Service Pack 3 (5.1.2600) |
| DirectX | 4.09.00.0904 |
| Disks | C: 874/932 GB free |
| Agent | 1.79.4 (the version that WROTE this record, at 2026-09-01 16:03) |
| Profile hash | `b65fa1fee4df292c` |

**No 3dfx silicon.** `Enum\PCI` carries no `VEN_121A` key, and a physically fitted card enumerates there even with no driver bound - so this is the decisive read, not merely "undriven".

Network: 192.168.1.133 - MAC 00-0C-41-E8-4F-CE

---

## 192.168.1.143 - 1GHZ

**As this box reported itself at 2026-09-01 16:10** (1 h ago). Every value below is that snapshot; restart its agent to take a fresh one.

> Athlon K7: family 6 model 2 is an AMD, NOT a Pentium III, and it has NO SSE. The Voodoo5 5500 is the SECOND adapter, behind the card driving the panel. NO SSE2, so UT99 469e and Halo are refused and there is no route to UT99 multiplayer here.

| field | value |
|-------|-------|
| CPU | AMD Athlon(tm) Processor, 1000 MHz |
| CPU id | family 6 model 2 stepping 2, vendor `AuthenticAMD` |
| Instruction set | fpu, mmx, cmov, 3dnow |
| RAM | 511 MB |
| Display GPU | NVIDIA GeForce 6800 (`10DE:0041`), 128 MB, sm3.0 |
| GPU driver | 7.1.8.9 (4-1-2005) |
| Display mode | 1280x1024 (registry) |
| OS | Windows XP Service Pack 3 (5.1.2600) |
| DirectX | 4.09.00.0904 |
| Disks | C: 133/224 GB free |
| Agent | 1.79.4 (the version that WROTE this record, at 2026-09-01 16:10) |
| Profile hash | `65a2b927575eaa82` |

**Video adapters on this box** - the one attached to the desktop is the one games run on; the others are fitted, or are stale class keys for cards that are not:

| instance | adapter | PCI | driver | attached to desktop |
|----------|---------|-----|--------|---------------------|
| 0000 | NVIDIA GeForce 6800 | `10DE:0041` | 7.1.8.9 | **yes** |
| 0001 | AMIGAMERLIN 3.1-R11 For Voodoo 5 5500 AGP | `121A:0009` | 5.1.2605.5 | no |

**3dfx silicon: 1 card.** From the PCI enumerator (`Enum\PCI`, `VEN_121A`), which is the only source a `Class=MEDIA` Voodoo 2 cannot hide from - it appears in no display-class scan at all.

- `VEN_121A&DEV_0009&SUBSYS_0002121A&REV_01` - AMIGAMERLIN 3.1-R11 For Voodoo 5 5500 AGP (1 instance)

Network: 192.168.1.143 - MAC 00-08-A1-03-52-C4

---

## 192.168.1.145 - DELL

**As this box reported itself at 2026-09-01 16:44** (1 h ago). Every value below is that snapshot; restart its agent to take a fresh one.

> DISPLAYCFG get reports the INACTIVE Intel HD, not the card driving the panel; cross-check a WINLIST Program Manager rect before believing a small mode here.

| field | value |
|-------|-------|
| CPU | Intel(R) Core(TM) i5-2400 CPU @ 3.10GHz, 3092 MHz x4 |
| CPU id | family 6 model 42 stepping 7, vendor `GenuineIntel` |
| Instruction set | fpu, mmx, cmov, sse, sse2, sse3, ssse3, sse4.1 |
| RAM | 3316 MB |
| Display GPU | NVIDIA GeForce 8400GS (`10DE:10C3`), 512 MB, sm3.0 |
| GPU driver | 6.14.13.4052 (7-2-2014) |
| Display mode | 1920x1080 (registry) |
| OS | Windows XP Service Pack 3 (5.1.2600) |
| DirectX | 4.09.00.0904 |
| Disks | C: 124/224 GB free |
| Agent | 1.79.4 (the version that WROTE this record, at 2026-09-01 16:44) |
| Profile hash | `bb7a13d03d67b0b4` |

**Video adapters on this box** - the one attached to the desktop is the one games run on; the others are fitted, or are stale class keys for cards that are not:

| instance | adapter | PCI | driver | attached to desktop |
|----------|---------|-----|--------|---------------------|
| 0000 | Intel(R) HD Graphics Family | `8086:0102` | 6.14.10.5337 | no |
| 0001 | NVIDIA GeForce 8400GS | `10DE:10C3` | 6.14.13.4052 | **yes** |

**No 3dfx silicon.** `Enum\PCI` carries no `VEN_121A` key, and a physically fitted card enumerates there even with no driver bound - so this is the decisive read, not merely "undriven".

Network: 192.168.1.145 - MAC D4-BE-D9-B9-BF-93

---

## 192.168.1.171 - NSC-5B996B81319

**As this box reported itself at 2026-08-31 08:17** (1 days ago). Every value below is that snapshot; restart its agent to take a fresh one.

> ANSWERS SLOWLY - use >=8s TCP timeouts or sweeps miss it entirely. Its Voodoo 2 is Class=MEDIA and appears in no display-class scan; see the accelerators list below.

| field | value |
|-------|-------|
| CPU | Intel(R) Pentium(R) 4 CPU 2.80GHz, 2793 MHz |
| CPU id | family 15 model 4 stepping 1, vendor `GenuineIntel` |
| Instruction set | fpu, mmx, cmov, sse, sse2, sse3 |
| RAM | 509 MB |
| Display GPU | Intel(R) 82865G Graphics Controller (`8086:2572`), 96 MB, fixed |
| GPU driver | 6.14.10.4396 (9-20-2005) |
| Display mode | 1280x1024 (registry) |
| OS | Windows XP Service Pack 3 (5.1.2600) |
| DirectX | 4.09.00.0904 |
| Disks | C: 40/93 GB free |
| Agent | 1.78.0 (the version that WROTE this record, at 2026-08-31 08:17) |
| Profile hash | `efb240b3f32bb482` |

**3dfx silicon: 2 cards.** From the PCI enumerator (`Enum\PCI`, `VEN_121A`), which is the only source a `Class=MEDIA` Voodoo 2 cannot hide from - it appears in no display-class scan at all.

- `VEN_121A&DEV_0002&SUBSYS_00000000&REV_02` - Voodoo2 3D Accelerator (2 instances)

Network: 192.168.1.171 - MAC 00-13-20-7B-2D-45

---

## 192.168.1.240 - USER-41EA3B3330

**As this box reported itself at 2026-09-01 16:42** (1 h ago). Every value below is that snapshot; restart its agent to take a fresh one.

> The disk-constrained box - check its free space here before staging anything large. Also prone to a game leaving the desktop at 640x480.

| field | value |
|-------|-------|
| CPU | AMD Athlon(tm) 64 Processor 3300+, 2403 MHz |
| CPU id | family 15 model 12 stepping 0, vendor `AuthenticAMD` |
| Instruction set | fpu, mmx, cmov, 3dnow, sse, sse2 |
| RAM | 1022 MB |
| Display GPU | RADEON X800 Series (`1002:4A4B`), 256 MB, sm2.0 |
| GPU driver | 8.593.100.0 (2-10-2010) |
| Display mode | 1920x1080 (registry) |
| OS | Windows XP Service Pack 3 (5.1.2600) |
| DirectX | 4.09.00.0904 |
| Disks | C: 1/74 GB free |
| Agent | 1.79.4 (the version that WROTE this record, at 2026-09-01 16:42) |
| Profile hash | `cab829af7a354f22` |

**Video adapters on this box** - the one attached to the desktop is the one games run on; the others are fitted, or are stale class keys for cards that are not:

| instance | adapter | PCI | driver | attached to desktop |
|----------|---------|-----|--------|---------------------|
| 0000 | RADEON 9800 XT | `1002:4E4A` | 8.593.100.0 | no |
| 0001 | RADEON 9800 XT - Secondary | `1002:4E6A` | 8.593.100.0 | no |
| 0002 | RADEON X800 Series | `1002:4A4B` | 8.593.100.0 | **yes** |
| 0003 | RADEON X800 Series Secondary | `1002:4A6B` | 8.593.100.0 | no |

**No 3dfx silicon.** `Enum\PCI` carries no `VEN_121A` key, and a physically fitted card enumerates there even with no driver bound - so this is the decisive read, not merely "undriven".

Network: 192.168.1.240 - MAC 00-11-2F-E4-77-F5

> **Clock skew:** this box's own clock reads 2026-09-01 13:42:17, 181 minutes behind the time the record landed here. Staleness above is judged by this host's clock, not the box's.

---

## 192.168.1.246 - ADMIN-PC

**As this box reported itself at 2026-09-01 16:45** (1 h ago). Every value below is that snapshot; restart its agent to take a fresh one.

> The only Windows 7 machine on the fleet.

| field | value |
|-------|-------|
| CPU | Intel(R) Core(TM) i5-2400 CPU @ 3.10GHz, 3093 MHz x4 |
| CPU id | family 6 model 42 stepping 7, vendor `GenuineIntel` |
| Instruction set | fpu, mmx, cmov, sse, sse2, sse3, ssse3, sse4.1 |
| RAM | 3317 MB |
| Display GPU | AMD Radeon HD 5450 (`1002:68F9`), 512 MB, sm3.0 |
| GPU driver | 15.200.1062.1004 (8-3-2015) |
| Display mode | 1920x1080 (registry) |
| OS | Windows 7  (6.1.7600) |
| DirectX | 4.09.00.0904 |
| Disks | C: 141/224 GB free |
| Agent | 1.79.4 (the version that WROTE this record, at 2026-09-01 16:45) |
| Profile hash | `7cf5bd34eefa68a7` |

**Video adapters on this box** - the one attached to the desktop is the one games run on; the others are fitted, or are stale class keys for cards that are not:

| instance | adapter | PCI | driver | attached to desktop |
|----------|---------|-----|--------|---------------------|
| 0000 | Standard VGA Graphics Adapter | `0000:0000` | 6.1.7600.16385 | no |
| 0001 | AMD Radeon HD 5450 | `1002:68F9` | 15.200.1062.1004 | **yes** |

**No 3dfx silicon.** `Enum\PCI` carries no `VEN_121A` key, and a physically fitted card enumerates there even with no driver bound - so this is the decisive read, not merely "undriven".

Network: 192.168.1.246 - MAC 78-2B-CB-A5-B4-70

---

## Records with no roster entry

These machines published a record but are not in `scripts/fleet/fleet-roster.txt`. That is information, not an error - a new box has appeared. Add it to the roster so it can ever be reported missing.

- `N5R5L9.json` - N5R5L9, 192.168.1.243, last seen 1980-01-04 00:02
- `NSC-9871C0E9964.json` - NSC-9871C0E9964, 192.168.1.124, last seen 2026-09-01 10:56
- `NSC-CABE14B7486.json` - NSC-CABE14B7486, 192.168.1.184, last seen 2026-09-01 10:59
- `XPBUILD.json` - XPBUILD, 10.0.2.15, last seen 2026-09-01 14:16

