# Retro fleet PXE server (proxyDHCP + TFTP)

Network-boot installer host for the fleet. Currently serves a **Windows XP SP3
network install** (RIS-style: `startrom.n12` -> `ntldr`/setupldr -> `winnt.sif` ->
SMB source), so a machine with no optical drive can be installed over the LAN.

```
target PC  --DHCP DISCOVER(PXEClient)-->  router DHCP  (gives the IP)
                                     \->  whitebeast   (gives next-server + boot file)
target PC  --TFTP-->  startrom.n12, ntldr, ntdetect.com, winnt.sif
target PC  --SMB1-->  \\192.168.1.122\files\Files\OS\XPSP3-PXE   (the CD contents)
```

## Where it runs, and why not in WSL

**It runs natively on whitebeast's Windows side (192.168.1.249), NOT in WSL.**
WSL2 here is NAT'd (172.19.x), so it never sees the fleet's broadcast domain -
a PXE client's DHCP DISCOVER would never reach it, and `netsh portproxy` cannot
help because it is TCP-only while DHCP/TFTP are UDP. Same rule as the game
servers (see `scripts/game-servers/README.md`).

- service: scheduled task **`RetroPXE`**, runs `pythonw.exe pxe_server.py` as
  SYSTEM at startup (`install-task.ps1`)
- runtime dir: `C:\development\pxe\` (TFTP root + log; the boot binaries come
  from the XP CD and are deliberately NOT in git)
- log: `C:\development\pxe\pxe_server.log`
- firewall: UDP 67 / 69 / 4011 inbound (`setup-firewall.ps1`)

## proxyDHCP, not DHCP

`pxe_server.py` never hands out IP addresses. It answers only packets carrying
vendor class `PXEClient`, with `yiaddr = 0.0.0.0`, `siaddr = <this host>`, the
boot file name, and PXE option 43 (discovery control 0x07). The LAN's real DHCP
server keeps doing addressing, so this is safe to leave running.

## Rebuilding the XP payload

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
| `pxe_config.json` | server IP, TFTP root, boot file, log path |
| `make-xp-source.ps1` | builds the XP payload from the ISO |
| `install-task.ps1` | registers the `RetroPXE` startup task |
| `setup-firewall.ps1` | inbound UDP rules |
| `winnt.sif.template` | reference copy of the generated `winnt.sif` |

Tests: `tests/python/test_pxe_server.py` (packet building, TFTP path safety).
