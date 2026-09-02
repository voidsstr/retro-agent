# Case Study 003: EPoX nForce2 — XP Installs Perfectly and Will Not Boot (STOP 0x7B)

## Problem Statement

An EPoX EP-8RDA (nForce2, Athlon XP) PXE-booted the fleet XP SP3 image, ran text-mode
setup and GUI setup to completion, rebooted — and never reached a desktop. Three full
installs failed this way in one session, each costing ~56 minutes. (The log holds 17 PXE
sessions on this MAC across seven days; the four below are the ones that isolate the bug.)
Note the first failed at the **MBR stage**, not with 0x7B — the failure mode changed as
variables were removed, and that is what located it.

The failure mode changed as variables were removed, which is what eventually located it:

| # | disk / controller | outcome |
|---|---|---|
| 1 | 250 GB behind a **SATA-to-IDE adapter**, onboard nForce2 IDE | install completed → **"Error loading operating system"** |
| 2 | 6 GB **native IDE**, onboard nForce2 IDE | install completed → **STOP 0x0000007B** (user-confirmed) |
| 3 | same disk, **slim** answer file (no `$OEM$`) | install completed → ~58 s boot loop |
| 4 | same disk on a **Promise Ultra 66 PCI IDE card**, onboard IDE disabled in BIOS | **boots** |

The machine is now [192.168.1.124 / NSC-9871C0E9964](../machines/192.168.1.124-NSC-9871C0E9964.md).

## Environment

- **Board**: EPoX EP-8RDA, nForce2, AMD Athlon XP 1800+, 511 MB RAM, Phoenix-Award BIOS v6.00PG
- **Onboard IDE**: `PCI\VEN_10DE&DEV_0065` (every onboard device reads `SUBSYS_10001695`; `0x1695` is EPoX)
- **NIC**: Intel PRO/100 (`PCI\VEN_8086&DEV_1229`), **physically moved from the Gateway 550**
- **Add-in card (the fix)**: Promise Ultra 66, `PCI\VEN_105A&DEV_4D38` (PDC20262)
- **Disk**: Quantum Fireball CR6, 6,448,619,520 bytes
- **Server**: `retro-pxe.service` on 192.168.1.132, image `XPSP3-FLEET` on the NAS (read-only mount)

## The root cause

`I386/TXTSETUP.SIF` binds the nForce2 IDE controller to NVIDIA's IDE driver:

```
[HardwareIdsDatabase]
PCI\VEN_10DE&DEV_0065 = "nvatabus"      <- nForce2 MCP IDE
PCI\VEN_10DE&DEV_0085 = "nvatabus"
PCI\VEN_10DE&DEV_008E = "nvatabus"
```

**We added that.** `I386/txtsetup.sif.preinject` — the pre-injection backup — contains
zero occurrences of `DEV_0065`. Stock XP has no such entry and falls through to the
generic class binding `PCI\CC_0101 = "pciide"`, which is still present in both files.
`scripts/pxe/inject-massstorage.py` introduced it as part of a bulk mass-storage import.

The Promise path is **mostly, but not entirely, stock** — and the difference matters:

```
preinject (stock)   PCI\VEN_105A&DEV_4D38&CC_0180 = "ultra"
live                PCI\VEN_105A&DEV_4D38&CC_0180 = "ultra"     <- stock
live                PCI\VEN_105A&DEV_4D38         = "Ultra"     <- ADDED by our injector
```

`ultra = ultra.sys,4` in `[SCSI.Load]` *is* stock. But the injector also added a bare
hardware id, and — the part nobody checked until afterwards — the image carries **two
different `ultra.sys` binaries**: the stock one inside `ULTRA.SY_` (extracts to 36,736
bytes) and a loose 35,538-byte copy from the driverpacks. setupldr fetched the stock
compressed one; which binary ends up installed on disk has not been verified. That is the
same hazard as the three rival `nvatabus.sys` builds below — an open check, not a footnote.

So "attempt 4 ran on Microsoft's tested path" is **too strong**. What holds is that the
Promise controller's binding is far closer to stock than the nForce2 one, which we
invented outright.

### What is proven, and what is not

Be careful repeating this. **Proven**: the nForce2 binding is ours, not stock; the machine
boots with the disk on the Promise card. **Not proven**: that
`nvatabus` is specifically and solely to blame. Attempt 4 changed several variables at
once — different controller, different driver, a different option-ROM INT 13h path, and a
freshly repartitioned disk. Nobody has yet run the controlled experiment (same disk, same
onboard controller, `nvatabus` binding removed). Until someone does, treat "nvatabus on
nForce2 breaks the boot" as *strongly consistent with the evidence*, not established.

## Why the earlier theories were wrong

Three plausible explanations were pursued and refuted. They are recorded because each
looked convincing and cost real time.

**"`Repartition` in `[Unattended]` is inert because we have no RIS OSChooser."** Wrong on
the second half. `Repartition` does belong in `[RemoteInstall]`, not `[Unattended]` — that
part holds — but text-mode setup *does* read it: `setupdd.sys` parses
`UseWholeDisk`/`Repartition`/`RemoteInstall` as an adjacent string triple. Do not describe
these keys as no-ops.

**"`retroagent.reg`'s 494-entry `DevicePath` let PnP swap in a different `nvatabus.sys`."**
The `$OEM$` tree does stage three different builds of that driver — `C:\D\M041` (100,736
bytes), `M042` (88,960), `M043` (105,344) — against the 100,736-byte copy in `I386`, and
all three sit in the PnP search path. A genuine hazard, and worth fixing on its own merits.
But it was not this bug: attempt 3 ran with **no `$OEM$` at all** and failed identically.

**"The slim profile will be much faster."** Text-mode went 36.8 min → 31.9 min. Dropping
2.48 GB and 17,981 files saved about five minutes out of fifty-six. The SMB copy was never
the bottleneck on this hardware.

## Reading the PXE log: success vs failure

`pxe_server.log` records only TFTP and BINL. Everything after `mrxsmb.sy_` happens over
SMB to the NAS and is invisible here, so the log is read by *timing and repetition*, not
by content.

**The log carries no dates** — every line is `[HH:MM:SS]` only, and it spans seven days
with 17 PXE sessions on this MAC alone. Before comparing any two timestamps, establish
which day each belongs to: walk the file for backwards time-jumps, and anchor against the
epoch value in `/srv/retro-pxe/pxe_state.json`. Durations below are measured **last TFTP
GET → next PXE contact**, which is what isolates text-mode setup.

| pattern | meaning |
|---|---|
| ~133–176 GETs ending at `mrxsmb.sy_` | text-mode boot phase **completed** — always looks like this, success or not |
| silence for 25–40 min, then one `HOLD` burst | setup running, then the normal post-setup reboot — **healthy** |
| `HOLD` bursts repeating every **~58 s** | **BSOD boot loop** |
| a `HOLD` burst, then lasting silence | consistent with **booted** — but confirm on the machine; the log cannot tell you |

Worked example from this incident — the same machine, two runs:

```
[23:10:34] OFFER, winnt.sif 1961 B      full profile
[23:11:28] boot phase ends (mrxsmb.sy_)
[23:48:14] HOLD  -> 36.8 min text-mode, rebooted
[00:13:23] HOLD  -> BSOD 0x7B

[09:48:12] OFFER, winnt.sif 1219 B      slim profile + Promise card
[09:48:54] GET ultra.sy_                (fetched on EVERY run — not a signal)
[10:37:37] HOLD, then quiet             -> booted
```

**There is no server-side signal that the Promise card is present.** An earlier draft of
this document claimed `ultra.sy_` in the boot phase proved it. That is wrong, and worth
understanding because the mistake is easy to repeat: `[SCSI.Load]` is fetched
**unconditionally** — every miniport, every run. `ultra.sy_` appears 106 times in this log
across nine different machines, including on this target from runs days before the Promise
card was fitted, and including every failing run.

The same is true of `nvatabus.sys`, which is fetched on the successful run too. Neither
file appearing in the log says anything about which controller owns the boot disk.

What the log *can* tell you: a BINL line is a genuine per-machine query and does confirm
the NIC match; reaching `mrxsmb.sy_` confirms the boot phase completed. For the storage
controller you must read the machine — text-mode setup lists "Promise Technology Inc.
Ultra IDE Controller", and the Promise option ROM lists attached drives during POST.

### Success does not answer ping

The slim profile omits `retroagent.reg`, so the XP firewall stays on and blocks ICMP. A
booted machine looks dead to `ping`. Check TCP 9898 (the agent), or the ARP table.

## Two failures that were NOT this bug

**"INF file txtsetup.sif is corrupt or missing, status 21"** — the NAS was down. TFTP
serves the image through the symlink `/srv/retro-pxe/tftp/Files → /mnt/retro-share/Files`;
when the mount dies, `txtsetup.sif` cannot be resolved and setupldr aborts with a message
that sounds like media corruption. Check `ping 192.168.1.122` and `ls /mnt/retro-share`
before suspecting the image. The file was byte-identical throughout
(md5 `9421a64e9f578cf26c54f371e4954a77`).

**"Bad or missing server discovery list" (PXE-E77)** — almost always the **boot hold**,
not a wire problem. A held MAC gets no reply at all (`pxe_server.py`, the `HOLD` branch
`continue`s), and the client's ROM reports it as a discovery failure. Check
`--list-holds` first.

## Do not trust the MAC-to-machine mapping in this repo

`00:d0:b7:40:96:a9` is called "the Gateway 550" in `scripts/pxe/pxe_server.py:139` and
`tests/test_pxe_boot_hold.py:11`. That was true when those lines were written. The NIC was
later moved into the EP-8RDA, so during this investigation the repo confidently described
the target as a 440BX Intel box — which would have made the entire nForce2 line of enquiry
irrelevant. It cost a diagnosis cycle and nearly sent the fix in the wrong direction.

Cards move between boards on this fleet. A MAC identifies a **NIC**, not a machine. Confirm
the board from the POST screen, or from `SUBSYS_` on the onboard devices, before trusting a
comment.

## If you do not have a Promise card

The targeted fix is to delete the three injected nForce2 lines from
`[HardwareIdsDatabase]` so the controller falls back to stock `pciide`. Keep the nForce3/4
mappings (`0035`, `0053`, `0054`, `0055`, …) — other fleet boards use them.

The image mount is **read-only**, and there is no sudo on the PXE host, so it cannot be
edited in place. A delivery mechanism was verified during this investigation and is
available: **serve a modified `txtsetup.sif` from the writable TFTP root.** Replace
`/srv/retro-pxe/tftp/Files` with a real four-level directory tree
(`Files/OS/XPSP3-FLEET/I386/`) that symlinks every other entry back to the NAS and holds
one real, edited `txtsetup.sif`.

This works because of how the file is consumed, which was established by disassembly
rather than assumption:

- `[HardwareIdsDatabase]` is read **only by setupldr** (`ntldr`), over TFTP. The string
  does not appear in `setupdd.sys` at all.
- Text-mode setup never re-reads `txtsetup.sif` from the SMB source — `setupdd.sys` carries
  no `i386\txtsetup.sif` path literal, and the wire log shows exactly one fetch per install.
- The TFTP resolver does not resolve symlinks against its root and rejects `..` before
  joining, so a symlink farm is served correctly and adds no traversal risk.

Replaying all 155 filenames from a real boot through the resolver against both trees
produced identical results except for the one overridden file.

## Recommended procedure for the remaining EPoX boards

1. Fit a **Promise Ultra 66** and put the boot disk on it.
2. **Disable the onboard IDE in BIOS**, so `nvatabus` has no disk to own.
3. Keep the disk **well under 137 GB** — these BIOSes are 28-bit LBA.
4. Prefer a **native IDE** disk. A SATA-to-IDE adapter was present in attempt 1, the only
   run that failed at the MBR rather than in the kernel; it has never been cleared.
5. PXE boot and watch for `ultra.sy_` in the first minute. If it is absent, stop — the card
   is not being seen, and the install will fail an hour later.
6. Let the post-setup reboot fall through to disk. Do not press F12, and do not `--release`
   the hold; that hold is what stops it reinstalling over itself.
