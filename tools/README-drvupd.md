# drvupd.exe - headless driver install on a fleet box (devcon replacement)

The fleet boxes have no `devcon.exe`, and XP has no CLI way to rescan the PnP
tree or bind an INF to a device. `drvupd.c` is that missing tool:

    i686-w64-mingw32-gcc -O2 -o drvupd.exe tools/drvupd.c -lsetupapi -lcfgmgr32

    EXECW 420 C:\path\drvupd.exe "C:\path\driver.inf" "PCI\VEN_1102&DEV_0002&CC_040100"

It does a synchronous `CM_Reenumerate_DevNode` on the root devnode (so a freshly
fitted card is enumerated), then
`newdev!UpdateDriverForPlugAndPlayDevicesA(..., INSTALLFLAG_FORCE)`. It prints
`INSTALLED ...` / `FAILED err=N` and needs no UI and usually no reboot.

Traps, all of which bit during the SB Live! install on .240 (2026-08-25):

- **Pass an HWID the INF actually lists**, not the device's exact subsystem ID.
  Creative's `wdma_emu.inf` only lists `PCI\VEN_1102&DEV_0002&CC_040100`; the
  card's exact `SUBSYS_80651102` appears in no INF on the box. Check the INF's
  Models section first. Quote the HWID so `cmd` does not eat the `&`.
- **Silence the unsigned-driver dialog first** or the EXEC blocks forever on an
  invisible prompt:
  `reg add "HKLM\Software\Microsoft\Driver Signing" /v Policy /t REG_BINARY /d 00 /f`.
- **Verify via the Enum node**, not the installer's exit code: a bound device
  gains `Service` + `Driver` and its `ConfigFlags` drops to 0.
- A card's secondary function is a separate devnode (SB Live's game port is
  `PCI\VEN_1102&DEV_7002`) - install it too, from its own INF.

Fleetbook recipe: `headless-driver-install-on-xp-with-no-devcon-drvupd-exe-setu`
(#20), which also covers carving the MSCF cab out of a vendor SFX that
`expand.exe` refuses, and extracting it on the box with `extrac32 /Y /E /L`.
