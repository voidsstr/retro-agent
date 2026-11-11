# Sound and video drivers: readiness audit, 2026-11-11

Read-only audit of the 10 boxes that were switched on (one agent per box; raw
evidence is in `audits.json` and the `audit-*` / `driver-audit-*` /
`drivers-110` folders). 3dfx drivers are reported only and were not changed, per
the agent's 3dfx rule.

## Needs action

| box | what | why it matters | recommended action |
|---|---|---|---|
| **.186** ADMIN-PC | **No display driver.** GeForce GTX 1650 (TU117) runs on Windows' Standard VGA stub under **Windows 7 32-bit RTM**. | Every 3D title fails. Turing has **no 32-bit Windows driver** (NVIDIA's 32-bit line ended at R390), so nothing can be installed on this OS as it stands. | Operator choice. **A:** reinstall Windows 7 SP1 **x64** with the SHA-2 updates (KB4474419 + KB4490628), then NVIDIA 474.30 x64. **B:** keep Win7 x86 and fit a card with a 32-bit Win7 driver: the 7900 GT/GTO it ran until 10-15 (NVIDIA 307.83 Win7 x86, *not* on the share), the 9400 GT (342.01 x86, local copy in `.claude/dl-nv34288/`), or anything Pascal or older (391.35 x86). Don't force any INF. |
| .186 | Win7 **not activated**: about 3 days of grace left, ending around **2026-11-14**. | After that, "not genuine" nags and a black wallpaper can steal focus from fullscreen games. Logon is not blocked. | Activate it (operator), or activate the reinstall if you choose route A. |
| .186 | Sound: the VIA VT1708S onboard driver works, but the analog "Speakers" output reads **unplugged**. Only S/PDIF and HDMI outputs exist, so the box is silent. | No game audio. | First plug speakers into the rear green jack and make "Speakers" the default device. Only if a connected jack still reads unplugged, install VIA HD Audio 6.0.01.7400 (`Drivers/Asus P5QL EPU/Audio/...viahdb1.inf`, HDVista32). |
| .186 | ~~Auto-logon broken~~: DefaultPassword was missing, so Windows reset AutoAdminLogon at boot (it sat at the logon screen for 24.5 h after 11-10). | Any reboot, including a driver or GPU change, strands the box with no agent. | **FIXED 2026-11-11:** `net user admin password` + DefaultPassword=password, read back (fleetbook change #254). Not yet proven across a reboot. |

## Leave alone: correct and stable (do not update)

| box | video | sound | note |
|---|---|---|---|
| .110 | GeForce 7800 GS, ForceWare 93.71 | SoundMAX AC'97, ADI 5.12.1.7000 | Right driver, md5-identical to the image store, no crashes since the card went in. *Re-image risk:* PREFER.TXT has no entry for `DEV_00F5`. |
| .123 | Radeon HD 3850 AGP, Catalyst 13.4 legacy beta (8.970.100.0) | Realtek AC'97 5.10.0.6300 | The SATA controllers flagged "missing" are false positives (legacy boot miniports). Do **not** DRVUPDATE the boot controller. **No physical keyboard or mouse is attached:** someone has to plug them in before a person can play. |
| .124 | Voodoo 5 6000 on vcr-kmd 0.1.0.0 (ours, experimental) | nForce2 APU 6.14.462.0 | 3dfx: report-only. Direct3D titles stay supervised-only (vcr-kmd README). |
| .133 | GeForce4 Ti 4600, ForceWare 93.71 | SB Live! Value, Creative 5.12.2.252 | One nv4_disp 0xEA hang on 10-14 in ~86 days. If it recurs: check fan/seating, move the SB Live! off the AGP's interrupt, then A/B 71.89. |
| .143 | GeForce 6800, ForceWare **71.89, pinned on purpose** | VIA AC'97 in-box WDM | **Don't update.** The Athlon has no SSE, and 93.71 or newer kills every OpenGL game (c000001d in nvoglnt, 2026-08-29). The **Unreal Gold client bugchecks this box** (8 of 9 0xEA hangs), so keep it out of Unreal Gold. A ghost Voodoo5 5500 and AmigaMerlin leftovers make FLEETRES think .143 has Glide (FR_GLIDE=1). Cleaning them needs your explicit ALLOW3DFX request. |
| .171 | GeForce FX 5500 PCI, ForceWare 175.19 | SoundMAX AC'97, ADI 5.12.1.5246 | Fine. *Re-image risk:* PREFER.TXT maps `DEV_0326` to 71.89, not 175.19. |
| .184 | GeForce2 GTS, ForceWare 71.89 (end of line) | AWE64, Microsoft in-box WDM | Fine. |
| .240 | Radeon X1950 Pro, Catalyst 10.2 legacy (8.593.100.0) | SB Live!, Creative 5.12.2.252 | Fine. The likeliest stutter source here is background load (MSE real-time scanning, Windows Search, Automatic Updates, the EA download manager), not a driver. |
| .243 | Cirrus GD5436 (2D) + Voodoo 2 (3dfx 3.02.02 kit) | SB16 PnP, in-box | Fine. 3dfx: report-only. The agent has no supervisor on Win9x and has been lost twice (11-07/08) with Windows still up. |

## Hardware warnings found on the way (not drivers)

- **.124 system disk:** Seagate ST9320325AS returned an uncorrectable read at
  LBA 6,160,459 at the 11-08 cold boot, and SMART shows 7 reallocated sectors.
  Read SMART 197/198 with a full tool, keep an image of C:, and replace the disk
  if the count grows.
- **.184 system disk path:** 47 Disk 51 paging-read errors (a burst on 09-01,
  and 1 at today's boot), with 0 reallocated sectors. That pattern points at the
  cable, power or controller. Check the SATA cable and the Molex-to-SATA
  adapter.
