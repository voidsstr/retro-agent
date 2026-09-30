# Windows 98 SE build VM (86Box)

The Win9x twin of the XP build VM: install and test Win9x/DOS titles here, then
stage them - never on `.243`. Shaped like `.243`: ASUS P/I-P55T2P4 (430HX),
Pentium 166 (P54C), 64 MB, Cirrus Logic GD5436, **Voodoo 2** (4+4 MB), **SB16 at
A220 I5 D1 H5**, NE2000 PCI on SLiRP. The retro agent runs inside
(`W98BUILD`), forwarded to **127.0.0.1:19930** - every fleet tool works against it.

| file | what |
|---|---|
| `build-w98vm.sh [VMDIR] [ISO]` | the whole host-side build: disk, `\WIN98`, `MSBATCH.INF` (key from the vault), floppy-chaining MBR, setup floppy, 86Box config |
| `run-98.sh` | start it (`systemd-run --user --unit=w98box -p CPUQuota=200% -p MemoryMax=2G run-98.sh`); own Xvfb `:22` |
| `inject-agent.sh [VMDIR]` | put the agent into a STOPPED VM's disk + `WIN.INI run=FIRSTRUN.BAT`; `MSDOS.SYS AutoScan=2` so a hard reset never stops at ScanDisk |
| `sweep.py [--only ...]` | launch every game shortcut, photograph 86Box's own display at 12/30/50 s, close it (VM reset if the agent stops answering); contact sheets in `~/.retro-fleet/w98vm/sweep/` |
| `vmshot.py`, `vmkey.py` | capture the emulated screen / type into it (XTEST on Xvfb `:22`; `Alt_L+a`-style combos) |
| `flopmbr.S` | VM-only MBR that boots the floppy; stage 1's `FDISK /MBR` replaces it |
| `msbatch.inf.in`, `setup-*.sys/.bat`, `agent/` | the answer file (placeholder key), floppy startup, first-logon setup |

Screens: `~/retro-vm/86box/86box-shot.py out.png --display :22`; keys:
`~/retro-vm/86box/86box-key.py Return Alt_L+a text:... --display :22`.

## Build (measured 2026-09-30, ~70 min, 4 keystroke stops)

1. `bash scripts/vm/win98/build-w98vm.sh` (refuses if the disk exists).
2. Start it; press **F1** once at "CMOS checksum error" (new NVRAM only).
3. Setup runs unattended from `MSBATCH.INF` but still stops, prefilled, at
   **User Information** (Return), the **licence** (`Alt_L+a`, Return) and the
   **product key** (Return). The key is `fleet-win98se-product-key`
   (verified by this setup; the screenshot of that page shows it - do not keep it).
4. At the first **desktop**: delete `fdd_01_fn` from `86box.cfg`, shut down
   from Start - **"Stand by" is preselected, choose "Shut down"** - wait for
   "It's now safe to turn off", `systemctl --user stop w98box`,
   `bash inject-agent.sh`, start it, and log on once with a **blank
   password**. From then on it logs on and starts the agent by itself (proven:
   agent `REBOOT`, back in 62 s, no keys).

## The Voodoo 2 driver (so Glide titles are tested too)

`.243` runs the 3dfx **3.02.02** kit; the VM gets the same fleet copy
(`Files\Drivers\3DFX\Win9x\voodoo2-30202-fleet`, see its FLEET-README):
UPLOAD every file to `C:\WINDOWS\OPTIONS\CABS\`, `VOODOO2.INF` also to
`C:\WINDOWS\INF\OTHER\3DFXV2.INF` (MKDIR `INF\OTHER` first - a fresh
install has none, and UPLOAD creates no folders), set
`HKLM\...\CurrentVersion\Setup SourcePath` to that folder, delete
`INF\DRVIDX.BIN`/`DRVDATA.BIN`, `REGDELETE` the driverless
`Enum\PCI\VEN_121A&DEV_0002...` devnode and restart. **Measured 2026-09-30:**
unlike `.243`, the Add New Hardware Wizard came up and its database search
did NOT find the INF; "Specify a location" = `C:\WINDOWS\OPTIONS\CABS`
found "Voodoo2 3D Accelerator" and installed it with no prompt. HWPROFILE then
reports `glide: true`; `profile_hash` is unchanged (accelerators are not in it).

## Traps it cost to learn

- **86Box's S3 Trio64 goes BLACK under Win98's own S3 Trio32/64 driver** - the
  boot log shows `s3.vxd` loading fine and Windows running blind. Cirrus GD5436.
- The P55T2P4 boots **C: before A:**; a blank MBR is a silent hang after
  "Update ESCD Successfully". Hence `flopmbr.S`.
- `ShowEula=0` / `Display=0` do not suppress those three wizard pages.
- **An agent `REBOOT` after a long GAMESYNC hung at "Windows is shutting
  down."** (the SMB redirector, after an afternoon of dropped sessions through
  SLiRP). A VM reset is the answer - `AutoScan=2` lets ScanDisk fix without a key.
- **SLiRP's SMB path drops**: error 55 mid-read about once a minute on big
  files (GAMESYNC's resume handles it), and once the session died outright for
  ~20 min (error 53 everywhere, `net use` "Disconnected") while the NAS pinged
  fine. A later `GAMESYNC START` got through; the cause is not proven.
- mtools prints `181 541 424 bytes`; a naive parse reads `181`.
- The install ISO is `\DiskImages\Windows 98 SE\` on the NAS. The disc in `.110`
  has one unreadable sector (247853, in `TOOLS\RESKIT\SYSFILES\MSVBVM50.DLL`),
  so no exact image of it exists; see `scripts/fleet/cdimage/`.
