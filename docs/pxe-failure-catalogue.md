# PXE XP install — how it works, and every failure seen so far

A symptom-first reference for the fleet's PXE installer. Every entry here was hit on real
hardware and diagnosed to a root cause; the dates are when it bit.

The companion documents are [`scripts/pxe/README.md`](../scripts/pxe/README.md) (how to run
the thing) and [case study 003](case-studies/003-epox-nforce2-xp-pxe-0x7b.md) (one incident
followed end to end).

## How a PXE install actually works

Five stages. Knowing which one you are in eliminates most of this document.

```
1  proxyDHCP    .132 answers a PXEClient DISCOVER with next-server + startrom.n12.
                It never assigns an address; the router still does DHCP.
2  TFTP boot    startrom.n12 -> ntldr (really SETUPLDR.BIN) -> ntdetect.com -> winnt.sif
                -> txtsetup.sif -> ~130 driver files.  ALL over TFTP from .132.
3  BINL         setupldr asks UDP 4011 which NIC driver to load. The SERVER answers.
                The image is never consulted for this.
4  text-mode    setup switches to SMB1 against \\192.168.1.122\files and copies the OS.
                INVISIBLE to the PXE log. 15-50 min.
5  GUI setup    reboots, runs from disk, 12-35 min, then reboots into the installed OS.
```

**The PXE log only sees stages 1-3.** Stage 4 onward is SMB traffic between the client and
the NAS; the server has no visibility. Almost every "it stopped doing anything" is simply
stage 4 running normally.

### Reading the log

| pattern | meaning |
|---|---|
| ~130-180 GETs ending at `mrxsmb.sy_` | stage 2 **completed** — looks identical whether the install later succeeds or fails |
| silence 15-50 min, then one `HOLD` burst | stage 4 ran, machine rebooted — **healthy** |
| a second silence 12-35 min, then another `HOLD` burst | stage 5 ran — **healthy** |
| `HOLD` bursts repeating every **~55-58 s** | **BSOD boot loop** |
| `HOLD` burst then lasting silence | consistent with success — but confirm on the machine |

**The log carries no dates.** Every line is `[HH:MM:SS]`. It spans many days, so before
comparing two timestamps establish which day each belongs to (walk for backwards
time-jumps; anchor against the epoch value in `/srv/retro-pxe/pxe_state.json`). Measure
stage 4 as **last TFTP GET → next PXE contact**.

### Two things in the log that prove nothing

**Driver filenames in stage 2 are not evidence of hardware.** `[SCSI.Load]` is fetched
**unconditionally** — every miniport, every run, whatever is fitted. `ultra.sy_` appears
106 times across nine machines in this log, including on machines with no Promise card and
on every failing run. Same for `nvatabus.sys`. An earlier draft of our own docs claimed
`ultra.sy_` proved the Promise card was in use; it does not, and the mistake was repeated
from the identical `nvatabus` warning two paragraphs away.

A **BINL** line *is* meaningful — it is a per-machine query and confirms the NIC match.

**A machine reappearing 10-15 minutes after stage 2 is setup SUCCEEDING**, not looping.
Text-mode finishing and rebooting looks exactly like a failure from the server's side. See
`e60f121`, where six such cycles wiped a machine repeatedly.

---

## Failures at stage 1 — never gets a boot file

### "Bad or missing server discovery list" (PXE-E77)

**Almost always the boot hold, not the wire.** A held MAC gets *no reply at all* — the
`HOLD` branch in `pxe_server.py` does a bare `continue` — and the ROM reports the silence as
a discovery failure. Check `--list-holds` first.

The genuine option-43 form of this error was fixed on 2026-08-26: discovery control was
`0x07`, whose `0x04` bit means "accept only boot servers from PXE_BOOT_SERVERS", a list we
never send. It is `0x0b` now.

### The machine reinstalls itself unasked

The timed hold **always expires** (6 h). These boxes boot from the network first, so an
expired hold means the next reboot reimages a finished machine. Cost a completed ASUS
install on 2026-09-02.

Use the permanent blocklist:

```json
"never_offer": ["e0:cb:4e:26:ec:a0"]
```

`held()` checks it before any time logic. **Do not try to fake this with a future timestamp
in `pxe_state.json`** — with `retry_grace_seconds` at 0, a negative age satisfies
`age < self.grace` and the machine lands in the *retry* branch, getting re-offered up to
`max_retries`. The exact opposite of blocking it.

---

## Failures at stage 2 — TFTP boot

### "INF file txtsetup.sif is corrupt or missing, status 21"

**The NAS was down.** TFTP serves the image through the symlink
`/srv/retro-pxe/tftp/Files → /mnt/retro-share/Files`; when that mount dies the file cannot
be resolved and setupldr aborts with a message that sounds like media corruption. Check
`ping 192.168.1.122` and `ls /mnt/retro-share` before suspecting the image. Seen
2026-09-01; the file was byte-identical throughout.

### setupldr fetches ntdetect.com, then dies without asking for winnt.sif

The answer file is missing a **fatal** entry. Disassembly of `SETUPLDR` found exactly two
sites that raise "The entry %s in the [%s] section of the INF file is corrupt or missing":

- `[SetupData] SetupSourceDevice`
- `[UserData] ComputerName`

Everything else is optional *to setupldr*. A `[Data]`+`[SetupData]`-only file — which is
what `winnt.sif.template` looks like — cannot boot. Observed three times with a 302-byte
file: it died right after the `migrate.inf`/`unsupdrv.inf` probes, which is where the
`ComputerName` read sits.

**A rescue answer file therefore needs three sections**, and omitting `[Unattended]` is what
yields the interactive "Welcome to Setup" screen with `R` = Recovery Console:

```
[Data]        floppyless, msdosinitiated, OriSrc, OriTyp, LocalSourceOnCD
[SetupData]   OsLoadOptions, SetupSourceDevice
[UserData]    ComputerName = *
```

`setupldr` always requests the literal filename `winnt.sif` and the server has no per-MAC
rewriting, so a rescue file must temporarily *replace* the live one.

### "The operating system image does not contain the necessary drivers for your network adapter"

**It means the SERVER did not answer**, not that the image lacks drivers. Stage 3 is a BINL
NetCard Query to UDP 4011; the image is never consulted. See
`docs/` memory `pxe-xp-binl-netcard-query` and `scripts/pxe/binl.py`. Getting NCR string 1
wrong (it is the PnP **hardware id**, uppercase short form — not the driver filename) fails
silently for twenty seconds then bugchecks `STOP 0x000000BB`.

---

## Failures after install — the machine will not boot

All of these look identical from the server: a completed stage 2, then a ~55-58 s
`HOLD` loop. **The registry on the disk is the only way to tell them apart.** Pull the disk,
attach it over USB, and read the SYSTEM hive with `hivex`/`chntpw` — it takes five minutes
and beats an hour-long reinstall every time.

### STOP 0x0000007B — the four causes seen so far

`INACCESSIBLE_BOOT_DEVICE` means the kernel cannot reach the boot volume. Four distinct
root causes, all producing the same bugcheck:

**1. Our injected `nvatabus` binding on nForce2 (2026-09-01).**
`I386/TXTSETUP.SIF` maps `PCI\VEN_10DE&DEV_0065`/`_0085`/`_008E` to `nvatabus`.
`txtsetup.sif.preinject` has **zero** occurrences of `DEV_0065` — stock XP falls through to
`PCI\CC_0101 = "pciide"`. `inject-massstorage.py` added it. Fix: disk on a different
controller, or remove those three lines.

**2. `atapi` disabled (2026-09-02).** `pciide.sys` is 3 KB — it only enumerates the
controller. **`atapi.sys` (96 KB) is the miniport that reads the disk.** With
`Services\atapi\Start = 4` the channels never load. Fix: `Start = 0`.

**3. Missing CriticalDeviceDatabase channel entries (2026-09-02).** A normal XP install
has `primary_ide_channel` and `secondary_ide_channel` → `atapi`. Without them the
controller enumerates but nothing drives the channels. This is what survives a disk moving
between machines. Fix: the MergeIDE set, below.

**4. A boot-start driver whose file is missing.** Any service with `Start = 0` whose `.sys`
is absent bugchecks the same way. **Caught in our own work**: setting `intelide`, `viaide`,
`aliide`, `cmdide`, `toside` boot-start for portability, when five of them had no file on
that disk. Always verify every `Start = 0` service resolves to a real file.

**Last Known Good does not help** when a driver installer wrote to *both* control sets —
which NVIDIA's nForce package does. `LastKnownGood` then points at an equally broken set.
On a fresh install there is often only `ControlSet001` and `LastKnownGood = 1`, i.e. no
earlier config to return to at all.

### Making an image portable — MergeIDE

So a disk boots on any IDE controller, not only the one it was installed on:

```
CriticalDeviceDatabase          Service    ClassGUID
  primary_ide_channel           atapi      {4D36E96A-...}
  secondary_ide_channel         atapi      {4D36E96A-...}
  *pnp0600                      atapi      {4D36E96A-...}
  *azt0502                      atapi      {4D36E96A-...}
  gendisk                       disk       {4D36E967-...}
  pci#cc_0101                   pciide     {4D36E96A-...}
  pci#cc_0106                   atapi      {4D36E96A-...}

Services ... Start = 0 : atapi pciide intelide viaide aliide cmdide toside
```

**Every one of those miniports must have its `.sys` present** — extract missing ones from
`I386/<name>.sy_` with `cabextract`. See cause 4 above.

### "Error loading operating system"

**MBR stage, before any OS driver exists.** The MBR read the partition table, called INT 13h
for the boot sector, and failed. No storage-driver change can affect it. Causes are disk
addressing:

- **The 137 GB (28-bit LBA) barrier.** 2003-era BIOSes cannot address past it. A 250 GB
  disk taken whole installs fine and then fails here.
- **The Promise Ultra 66 (PDC20262) has the same limit** — it predates 48-bit LBA. Fitting
  the card does *not* raise the ceiling; only a Ultra133 TX2 or similar does.
- **Geometry mismatch** — a partition table written under one translation and read under
  another.

Keep boot partitions under ~120 GB on this hardware.

---

## Driver installs that fail on a running machine

`C:\WINDOWS\setupapi.log` records every attempt and names the real error. **Read it before
theorising** — it resolved two failures here that had been misdiagnosed twice each.

### Code 39, "cannot start", on an nForce2 NIC

Wrong driver **architecture**. NVIDIA ships two ethernet stacks and only one supports
nForce2:

| INF | architecture | claims `DEV_0066`? |
|---|---|---|
| `nvenetfd.inf` / `nvnb5032.inf` | NVENETFD + NRM | **no** — nForce3/4 |
| `nvenetxp.inf` (PreNRM) | NVENET | **yes** |

NVIDIA's own UDP 5.10 package puts the working one in `Ethernet/PreNRM/WinXP/`. Our
driverpacks' `nvnb5032.inf` *claims* `DEV_0066` but is the NRM stack; it loads
`nvefd2k.sys`, the service fails to start, code 39. Adding `nvnrm.sys` does not help — the
architecture is simply wrong.

### "The drivers are not installed" with no obvious error

```
#E279 Add Service: Failed to create service "NVENET".
      Error 1078: The name is already in use as either a service name
      or a service display name.
#I125 Installing NULL driver for "PCI\VEN_10DE&DEV_0066..."
```

A **duplicate DisplayName**, not a duplicate service name. A leftover `NVENETFD` service
held `"NVIDIA nForce Networking Controller Driver"` — byte-identical to the string
`nvenetxp.inf` wants for `NVENET`. Windows refuses, then installs a NULL driver, and the
device reports `ConfigFlags = 0x40` (`CONFIGFLAG_FAILEDINSTALL`). **Disabling the offending
service is not enough — the name is reserved either way.** Delete the service key.

### Install blocked, error 1168 "Element not found"

```
#E358 An unsigned or incorrectly signed file "...inf" ... blocked (server install).
      Error 1168
```

Driver signing policy. `HKLM\SOFTWARE\Microsoft\Driver Signing\Policy` was `01` = **Warn**.
Interactively Warn shows a "Continue Anyway" prompt; **PnP's non-interactive path has nobody
to click it, so Warn behaves as Block.** Set it to `00` (Ignore) — machine key, the
`Policies\...\Driver Signing\BehaviorOnFailedVerify` override, and per-user.

Our `winnt.sif` sets `DriverSigningPolicy = Ignore`, yet a machine still ended up at Warn.
Something after setup resets it. Check this first on any unexplained driver failure.

### Recovering a driver install offline

When the machine has no network, mount its disk and:

1. Place the INF in `WINDOWS\inf`, the `.sys` in `system32\drivers`, co-installer DLLs in
   `system32` (read the INF's `[SourceDisksFiles]` — it lists exactly what is required;
   a missing co-installer is a silent failure).
2. Neutralise competing INFs (`rename` them) and delete their stale `.PNF` caches.
3. Delete any service whose DisplayName collides.
4. Set the device's `ConfigFlags = 0x20` (`REINSTALL`) in **every** control set.
5. Point `DevicePath` at the driver folder.

On a machine that *does* have the agent, `DRVUPDATE <hwid> <inf>` does all of this properly
in one command — prefer it to hand-editing hives.

---

## Other traps

### `ERROR_NOT_ENOUGH_QUOTA` — "Not enough quota is available to process this command"

**A Windows resource error, not a licensing one.** Found on 2026-09-02 with
`SystemPages = 823296` — ~3.14 GB of system PTEs requested on a 32-bit kernel with 2 GB of
address space. The memory manager cannot satisfy it and the **paged pool** is what starves.
XP's own `hivesys.inf` ships `SystemPages = 0` (automatic); our tooling does not set it, so
something on the machine did. Fix: set it back to `0`.

The other classic cause is desktop-heap exhaustion —
`Session Manager\SubSystems\Windows`, `SharedSection=1024,3072,512`.

### Read the EVENT LOG before changing any resource setting

`ERROR_NOT_ENOUGH_QUOTA`, "the system directory is corrupted", stalls, and unexplained
bugchecks can all be one fault: **failing disk I/O**. XP records it plainly, and the log
is readable offline from the disk (`WINDOWS/system32/config/SysEvent.Evt`, legacy `.Evt`
format - no parser is installed here, but the record layout is simple enough to parse
directly).

On the EPoX/Voodoo box, 2026-09-02:

```
739x  Event 26  source ultra   \Device\Scsi\ultra1        (Promise driver, device error)
684x  Event 51  source Disk    "error during a PAGING operation"
  6x  Event 57  source Ftdisk  could not flush the transaction log
  3x  Event  9  source ultra   device did not respond within the timeout
```

1,438 errors over the life of the install. **Event 51 is literally a paging failure** -
paging backs the paged pool, so when it fails the next allocation reports
`ERROR_NOT_ENOUGH_QUOTA`. The activation wizard was not special; it was simply the next
thing to ask for memory.

Two registry "fixes" were applied before the log was read - `SystemPages` (genuinely
misconfigured at 823296) and the desktop heap (at stock). Neither could ever have worked,
because nothing was exhausted. **The log named the fault immediately and was on the disk
the whole time.**

The drive was then surface-tested over USB: 768 MB pagefile read end to end at 47 MB/s,
300 large system32 files all readable, zero kernel I/O errors. Same platters, clean on one
controller and 1,438 errors on another - so the fault was the path
(SATA drive -> IDE/SATA bridge -> Promise Ultra 66), not the media.

`ntfsclone` later found **13 cluster accounting mismatches** in `$Bitmap`, confirming the
I/O errors had done real filesystem damage rather than just setting a dirty flag.

### "The system directory is corrupted", clears after a reboot

The **NTFS dirty bit**. Overwhelmingly caused by pulling a USB-attached disk without
unmounting. `ntfs3` says so plainly in `dmesg`:

```
ntfs3(sdX1): volume is dirty and "force" flag is not set!
```

Fix: `sudo umount` then `sudo ntfsfix /dev/sdX1`, and let Windows' `autochk` run at the next
boot. **Always unmount before unplugging** — every unclean removal sets it again.

### A MAC does not identify a machine

`00:d0:b7:40:96:a9` is called "the Gateway 550" in `pxe_server.py:139` and
`tests/test_pxe_boot_hold.py:11`. That was true when written; the NIC was later moved into
an EPoX board, and during a 2026-09-01 investigation the repo confidently described the
target as a 440BX Intel box — which would have made the whole nForce2 line of enquiry
irrelevant. Confirm the board from POST or from `SUBSYS_` on onboard devices.

### The slim profile changes what "working" looks like

`OemPreinstall = No` skips the entire `$OEM$` tree, so there is no `cmdlines.txt`, no
`retroagent.reg`, no `C:\D`, **no agent, and no firewall-off**. A perfectly healthy machine
then does not answer `ping`, because XP's firewall blocks ICMP. Check TCP 9898 or ARP, never
ping alone. It also means injected driverpacks are never copied — injection and slim are
mutually exclusive.

### DriverPacks gaps are real

The packs have **no `DEV_8168`** (RTL8111/8168 PCIe — standard on LGA775 boards). ASUS's own
2009 package covers it; the ASUS `Netrtle.inf` is **UTF-16**, so an ASCII `grep` reports
zero hits on a 752 KB file, and its XP section is `[Realtek.NTx86.5.1]` — `.5.1` *is* NT 5.1.
Screening for the undecorated `[Realtek.NTx86]` discards the best driver.

---

## Diagnostic order that actually works

1. **Read the screen.** More time was lost this week to inferring machine state from reboot
   timing than to any single bug.
2. **`--list-holds`.** Most "server is broken" reports are the hold working.
3. **`ping 192.168.1.122` / `ls /mnt/retro-share`.** A dead NAS looks like corrupt media.
4. **`setupapi.log` on the target** for any driver failure.
5. **Pull the disk and read the SYSTEM hive** for any post-install boot failure. Five
   minutes, and it gives a definite answer where reinstalling gives another guess.
6. Only then change something — and change **one** thing.
