# Retro fleet PXE server (proxyDHCP + TFTP)

Network-boot installer host for the fleet. Currently serves a **Windows XP SP3
network install** (RIS-style: `startrom.n12` -> `ntldr`/setupldr -> `winnt.sif` ->
SMB source), so a machine with no optical drive can be installed over the LAN.

```
target PC  --DHCP DISCOVER(PXEClient)-->  router DHCP  (gives the IP)
                                     \->  .132         (gives next-server + boot file)
target PC  --TFTP-->  startrom.n12, ntldr, ntdetect.com, winnt.sif
target PC  --SMB1-->  \\192.168.1.122\files\Files\OS\XPSP3-PXE   (the CD contents)
```

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
