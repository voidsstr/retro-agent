# provisioning/dali - the real-DOS IPX kit for a Win9x box's DOSBox LAN games

Staged into a title's `DALI\` folder by `scripts/dosgames/stage_dali_lan.py`.

| file | what | source / licence |
|---|---|---|
| `DALI.EXE` | DALI v0.3 - a real-DOS IPX driver that joins a DOSBox IPX server over UDP | fragglet/dali v0.3 (`dali-0.3.zip`, 2020-02-13), GPL-3 - `COPYING.TXT`, source https://github.com/fragglet/dali |
| `DHCP.EXE` | mTCP DHCP client, the build shipped in `dali-0.3.zip` | mTCP (Michael Brutman), GPL-3 |
| `ASKIP.COM` | asks for the hosting machine's IP; prints `SET HOSTIP=` | ours: `scripts/dosgames/dali/askip.c` |
| `WBOOT.COM` | warm reboot after the game (a Crynwr packet driver cannot unload) | ours: `scripts/dosgames/dali/wboot.c` |
| `IPXCHK.COM` | INT 2Fh AX=7A00h - errorlevel 0 when an IPX driver is resident | ours: 64 bytes, built in the 2026-10-01 session (see FINDINGS) |

| `3C509.COM`, `NE2000.COM` | Crynwr packet drivers (3Com EtherLink III; NE2000 incl. the VM's PCI RTL8029) | Crynwr packet driver collection, GPL - the same builds `PLAY.BAT` uses (copied 2026-10-01 from the share's stale `Files/Utility/Retro Automation/dos-setup/C/DOSGAME/NET/`, which the canonical `Utility/Retro Automation` no longer carries) |

Every file is md5-pinned in `MD5SUMS.txt`; the generator refuses a mismatch.

Proven 2026-10-01: the Win98 build VM in REAL DOS (NE2000 PCI at E000/IRQ 10,
SLiRP) ran `DALI 192.168.1.123 213` against `.123`'s DOSBox `IPXNET
STARTSERVER`, listed `.123`'s Descent netgame and flew in it (both screens).
