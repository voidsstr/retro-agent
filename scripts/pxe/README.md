# Retro fleet PXE server (proxyDHCP + TFTP)

Network-boot installer host for the fleet. Currently serves a **Windows XP SP3
network install** (RIS-style: `startrom.n12` -> `ntldr`/setupldr -> `winnt.sif` ->
SMB source), so a machine with no optical drive can be installed over the LAN.

```
target PC  --DHCP DISCOVER(PXEClient)-->  router DHCP  (gives the IP)
                                     \->  .132         (gives next-server + boot file)
target PC  --TFTP-->  startrom.n12, ntldr, ntdetect.com, winnt.sif
target PC  --TFTP-->  \Files\OS\XPSP3-PXE\i386\txtsetup.sif       (see below)
target PC  --SMB1-->  \\192.168.1.122\files\Files\OS\XPSP3-PXE   (the CD contents)
```

## Every failure mode, symptom-first

[`docs/pxe-failure-catalogue.md`](../../docs/pxe-failure-catalogue.md) is the reference:
the five stages of an install, how to read this server's log, and every failure hit on real
hardware with its root cause and fix - PXE-E77, "txtsetup.sif is corrupt", the four distinct
causes of STOP 0x7B, driver installs that fail with code 39 / error 1078 / error 1168,
`ERROR_NOT_ENOUGH_QUOTA`, and the NTFS dirty bit.

Start there when something breaks. The short version of its diagnostic order:

1. read the screen
2. `--list-holds`  (most "the server is broken" reports are the hold working)
3. `ping 192.168.1.122`  (a dead NAS looks like corrupt media)
4. `setupapi.log` on the target, for anything driver-shaped
5. pull the disk and read the SYSTEM hive, for anything boot-shaped

## Check it before you walk to the machine

```bash
sudo python3 scripts/pxe/pxe_selftest.py      # or --tftp-only, no root needed
```

It performs the same two exchanges a boot ROM does and checks the fields that
decide whether the target boots. Worth running first every time, because **every
failure in this path is silent from the client end** - the machine prints
"DHCP." then "TFTP." and stops, naming nothing. Last full run: 13/13.

## Where it runs

**It runs on the Linux fleet host, 192.168.1.132.** The 2026-08-24 cutover
paused whitebeast, and this is the one tool that installs an OS on a machine
whose optical drive is dead, so it moved with everything else.

- service: **`retro-pxe.service`** - a SYSTEM unit, and it has to be: 67, 69 and
  4011 are privileged ports and a `--user` manager cannot bind them. It still
  does not run as root. `CAP_NET_BIND_SERVICE` is the only capability granted,
  `NoNewPrivileges=true`, and `ProtectSystem=strict` leaves `/srv/retro-pxe` as
  the single writable path.
- runtime dir: `/srv/retro-pxe/` (TFTP root + log). The boot binaries come off
  the XP CD and are deliberately **not** in git - `/srv` also keeps them out of
  the repo tree.
- log: `/srv/retro-pxe/pxe_server.log` - every OFFER and every TFTP GET.
- payload: `bash scripts/pxe/make-xp-source.sh`
- firewall: nothing to do here (ufw inactive, INPUT ACCEPT). On the Windows host
  it needed `setup-firewall.ps1`.

`server_ip` defaults to **`auto`** and is resolved at startup, rather than
hardcoded. A stale fleet address has caught this setup before and it fails
silently: the client is handed a next-server it cannot reach and just waits.

### The Windows host is still supported

`install-task.ps1`, `setup-firewall.ps1` and `make-xp-source.ps1` still work,
and the defaults follow the platform (`C:\development\pxe` there,
`/srv/retro-pxe` here), so the same `pxe_config.json` is correct on both. Do not
run BOTH at once on the same LAN - two proxyDHCP servers answering one DISCOVER
is a race, and which boot file wins is down to timing.

## proxyDHCP, not DHCP

`pxe_server.py` never hands out IP addresses. It answers only packets carrying
vendor class `PXEClient`, with `yiaddr = 0.0.0.0`, `siaddr = <this host>`, the
boot file name, and PXE option 43 (discovery control 0x07). The LAN's real DHCP
server keeps doing addressing, so this is safe to leave running.

## The product key lives in Key Vault, not in this repo

The XP media is RETAIL (`I386\SETUPP.INI` reads `Pid=76487000`; the trailing
`000` is the retail channel), so an unattended install needs a product key and
`FullUnattended` stops dead without one.

The key is in **`nsc-secrets-kv`** as **`fleet-winxp-pro-sp3-product-key`** -
the same vault and the same `fleet-*` naming `install/recover-credentials.sh`
uses for everything else, so a host rebuild recovers it with the rest.

```bash
PRODUCT_KEY="$(az keyvault secret show --vault-name nsc-secrets-kv \
    --name fleet-winxp-pro-sp3-product-key --query value -o tsv)" \
    WIPE=1 bash scripts/pxe/make-xp-source.sh
```

Never write the key into `pxe_config.json`, the template, or a commit. It is
passed in at build time and lands only in the generated `winnt.sif`.

**And know what that means: `winnt.sif` is served over TFTP with no
authentication.** Anyone on the fleet LAN can fetch it and read the key. That is
an accepted trade on an isolated LAN, but it is a real exposure and it is the
reason the key is not baked into the payload permanently - rebuild without
`PRODUCT_KEY` when you are done imaging and the file goes back to
`ProvideDefault` with no key in it.

### Failing over to another host

Everything needed is either in git or in Key Vault, so a rebuild is:

1. `git clone` this repo; the PXE server, builder, unit and self-test come with it.
2. Mount the NAS at `/mnt/retro-share` (the fstab entry uses
   `credentials=/etc/cifs-retro-share.creds`, `vers=2.0`, `nofail`,
   `x-systemd.automount`).
3. `apt-get install cabextract` - the boot files are CABs, not SZDD.
4. Pull the key from the vault and run `make-xp-source.sh` as above.
5. `sudo cp scripts/pxe/retro-pxe.service /etc/systemd/system/ && sudo systemctl enable --now retro-pxe`
6. `sudo python3 scripts/pxe/pxe_selftest.py` - 13/13 before you trust it.

Nothing is pinned to this machine: `server_ip` is `auto`, and the TFTP payload
is rebuilt from the NAS rather than copied. The one thing NOT recoverable from
the vault is the XP media itself, which lives on the NAS at
`Files/OS/XPSP3-PXE`.

**Do not run two PXE hosts on one LAN.** Two proxyDHCP servers answering one
DISCOVER is a race and which boot file wins is down to timing.

## Rebuilding the XP payload

On the Linux host:

```bash
bash scripts/pxe/make-xp-source.sh
```

It reads the already-expanded CD tree on the NAS, so no ISO mounting is needed.
Two traps it encodes:

- the compressed files are **CABs** (`MSCF` magic), not the SZDD that "expand"
  implies - `cabextract` is the tool and `msexpand` fails on them.
- `SETUPLDR.EX_` must land as the literal name **`ntldr`**. That is the name
  startrom asks TFTP for, and what it wants there is setup's loader, not an
  installed system's ntldr. Copy it under its own name and the boot dies with
  no useful message.

It generates `winnt.sif` inline from the same two parameters the PowerShell
sibling takes, so `winnt.sif.template` stays a reference copy of the result
rather than an input only one builder reads.

On the Windows host:

```powershell
powershell -ExecutionPolicy Bypass -File make-xp-source.ps1     # elevated
```
It mounts the XP ISO, expands `STARTROM.N1_` -> `startrom.n12` and
`SETUPLDR.EX_` -> **`ntldr`** (that name matters: startrom asks TFTP for
`ntldr`), copies `NTDETECT.COM`, writes `winnt.sif`, and robocopies the whole CD
to the NAS.

## setupldr does not switch to SMB when you expect

Loading `ntldr` is not the end of the TFTP phase. setupldr then wants
`txtsetup.sif`, and it fetches that over **TFTP as well** - taking the path out
of `winnt.sif` and dropping the UNC prefix. So a `winnt.sif` naming
`\\192.168.1.122\files\Files\OS\XPSP3-PXE` makes it ask TFTP for
`\Files\OS\XPSP3-PXE\i386\txtsetup.sif`, relative to the TFTP root.

If that path is not there the boot stops at **"txtsetup.sif is corrupt or
missing"**, which reads like a bad copy of the CD and is nothing of the sort.

`make-xp-source.sh` therefore symlinks the share tree into the TFTP root
(`/srv/retro-pxe/tftp/Files -> /mnt/retro-share/Files`). A symlink is enough:
the resolver rejects `..` but does not resolve symlinks against the root, and
its case-insensitive walk handles `i386` -> `I386` and `txtsetup.sif` ->
`TXTSETUP.SIF`.

## The install is unattended on a BLANK disk, and only on a blank disk

`AutoPartition = 1` installs to the first partition with enough room **that does
not already contain an installation of Windows**. On a blank disk that is the
free space and the install is hands-off. On a disk that already carries XP there
is no eligible partition, so text-mode setup falls back to the interactive
partition list - a `FullUnattended` answer file stopping on the one page it was
written to skip.

`Repartition` is the key that clears the disk, and it belongs in
**`[RemoteInstall]`**, not `[Unattended]`. In `[Unattended]` it parses without
complaint and is ignored, so it reads like it is working.

**It is NOT inert once placed correctly** - an earlier version of this file
claimed it needed the RIS Client Installation Wizard, which we do not implement.
That was wrong: `setupdd.sys` parses `UseWholeDisk`, `Repartition` and
`RemoteInstall` as an adjacent string triple, so text-mode setup reads them
directly.

```
[RemoteInstall]
    Repartition = Yes
    UseWholeDisk = Yes
```

**The default is destructive.** Omit the section, or the key, and a RIS install
deletes every partition anyway - so `WIPE=0` has to write `No` explicitly.
Leaving it out is not the cautious choice, it is the dangerous one.
`make-xp-source.sh` writes the section either way for that reason.

## Blocking a machine permanently

The timed hold ALWAYS expires (6 h), and these boxes boot from the network first - so an
expired hold means a finished install gets silently reimaged. That cost a completed install
on 2026-09-02.

For a machine whose install must be preserved indefinitely, add its MAC to `never_offer` in
`pxe_config.json` and restart the service:

```json
"never_offer": ["e0:cb:4e:26:ec:a0"]
```

`held()` checks that list before any time-based logic, and `--list-holds` prints a
PERMANENTLY BLOCKED section so it is visible rather than mysterious.

**Do not fake this with a future timestamp in `pxe_state.json`.** With
`retry_grace_seconds` at 0 a negative age still satisfies `age < grace`, so the machine
lands in the retry branch and is RE-OFFERED - the opposite of blocking it.

## nForce2 boards need the boot disk OFF the onboard IDE

`inject-massstorage.py` binds the nForce2 IDE controller to NVIDIA's driver:

```
PCI\VEN_10DE&DEV_0065 = "nvatabus"      # and _0085, _008E
```

**Stock XP does not do this** - `txtsetup.sif.preinject` has zero occurrences of
`DEV_0065`, and retail falls through to `PCI\CC_0101 = "pciide"`. An EPoX
EP-8RDA imaged this way installs perfectly and then fails to boot - **STOP
0x0000007B INACCESSIBLE_BOOT_DEVICE** on the runs with a native IDE disk, and an
MBR-stage "Error loading operating system" on one earlier run behind a
SATA-to-IDE adapter.

The fix that worked: a **Promise Ultra 66** PCI IDE card (`VEN_105A&DEV_4D38`)
with the boot disk on it and the onboard IDE disabled in BIOS. Promise support
is MOSTLY stock: `ultra = ultra.sys,4` and the `&CC_0180` hardware id are in
`txtsetup.sif.preinject`, though our injector added a bare `DEV_4D38` id and a
second, non-stock `ultra.sys` binary alongside the stock one.

**The log cannot tell you whether the card is in use.** `[SCSI.Load]` is fetched
unconditionally - every miniport, every run - so `ultra.sy_` and `nvatabus.sys`
both appear whatever hardware is fitted (`ultra.sy_`: 106 fetches, nine machines,
every failing run included). Confirm the controller on the machine instead: the
Promise option ROM lists attached drives at POST, and text-mode setup names
"Promise Technology Inc. Ultra IDE Controller".

Full write-up, including what is proven versus merely consistent and how to fix
this without a Promise card: [case study 003](../../docs/case-studies/003-epox-nforce2-xp-pxe-0x7b.md).

## A booted machine may not answer ping

The slim profile (`OemPreinstall = No`) never runs `cmdlines.txt`, so
`retroagent.reg` never merges, so the XP firewall stays on and blocks ICMP. A
successful install looks dead to `ping`. Check TCP 9898 or the ARP table
instead.

## Two log lines that are NOT faults

- `MISS BOOTFONT.BIN` - only used for East-Asian boot locales. English XP does
  not need it.
- `no ACK for OACK, aborting startrom.n12` - the Intel Boot Agent asks first
  with `tsize`, does not acknowledge that OACK, then immediately retries with
  `blksize` and succeeds. It costs one round trip and nothing else.

## The SMB1 trap

XP's text-mode setup speaks **SMB1 only**, and Windows 11 no longer ships an
SMB1 *server* - whitebeast therefore cannot host the install source itself. The
NAS (`MEDIASERVER`, 192.168.1.122) does, and it allows **null-session** access,
which is what setupldr needs since it has no credentials to offer. Verified from
an XP box with `net use \\MEDIASERVER\files /user:"" ""`.

## Booting a target machine

1. In the target's BIOS enable the onboard LAN option ROM / "Boot from network",
   or hit the one-time boot menu (usually F12) and pick the NIC.
2. It should show `DHCP.../ TFTP.` then "Windows Setup" in blue - that is
   setupldr running from `ntldr`.
3. Text-mode setup partitions/formats and copies from the NAS. When it reboots,
   set the boot order back to the hard disk (or just do not press F12 again).

Watch `pxe_server.log` while it boots - every DHCP OFFER and TFTP GET is logged,
so a failure tells you exactly how far it got.

**A run of 133 GETs ending at `mrxsmb.sy_` is text-mode setup SUCCEEDING**, not
dying. That is the end of the TFTP phase; setup then works over SMB for about
ten minutes and reboots to continue into GUI setup. The machine coming back to
PXE ~10 minutes later is that reboot, and the boot hold refusing it is the hold
doing its job - `--release` there re-images a box that had just finished. See
`e60f121`.

## Files

| file | what |
|---|---|
| `pxe_server.py` | the server: proxyDHCP (67 + 4011) + TFTP (69), stdlib only |
| `pxe_selftest.py` | proves the server would boot a machine, from the host |
| `retro-pxe.service` | systemd system unit for the Linux fleet host |
| `make-xp-source.sh` | builds the XP payload on Linux |
| `pxe_config.json` | server IP, TFTP root, boot file, log path |
| `make-xp-source.ps1` | builds the XP payload from the ISO |
| `install-task.ps1` | registers the `RetroPXE` startup task |
| `setup-firewall.ps1` | inbound UDP rules |
| `winnt.sif.template` | reference copy of the generated `winnt.sif` |

Tests: `tests/python/test_pxe_server.py` (packet building, TFTP path safety).
